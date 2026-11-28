/**
 * @file
 * @brief Windows sockets (Winsock 2).
 *
 * WSAStartup runs on every socket creation: it is reference counted, thread-safe and cheap, and
 * nothing calls WSACleanup (the process exit does).
 */
#include "os/os_socket.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

_Static_assert(sizeof(((os_addr_t*)0)->raw) >= sizeof(struct sockaddr_storage),
	       "os_addr_t too small");
_Static_assert(sizeof(os_socket_t) == sizeof(SOCKET), "os_socket_t is not a SOCKET");

static int
family_of(const os_addr_t* addr)
{
	return ((const struct sockaddr*)addr->raw)->sa_family;
}

/* Failure paths close the socket without losing the error the caller reports. */
static os_socket_t
fail_close(SOCKET s)
{
	int saved = WSAGetLastError();

	closesocket(s);
	WSASetLastError(saved);
	return OS_SOCKET_INVALID;
}

/* Nonblocking and not inherited by child processes (the Winsock equivalent of SOCK_CLOEXEC). */
static bool
prepare(SOCKET s)
{
	u_long on = 1;

	if (ioctlsocket(s, FIONBIO, &on) != 0)
		return false;
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
	return true;
}

static os_socket_t
open_socket(int family, int type)
{
	WSADATA	wsa;
	SOCKET	s;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return OS_SOCKET_INVALID;
	s = socket(family, type, 0);
	if (s == INVALID_SOCKET)
		return OS_SOCKET_INVALID;
	if (!prepare(s))
		return fail_close(s);
	if (type == SOCK_DGRAM)
	{
		/* Without this an ICMP port unreachable makes the next recvfrom fail with WSAECONNRESET. */
		DWORD	got;
		BOOL	off = FALSE;

		WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &got, NULL, NULL);
	}
	return s;
}

bool
os_ip_is_literal(const char* text)
{
	struct in_addr	addr4;
	struct in6_addr	addr6;

	if (!text)
		return false;
	return inet_pton(AF_INET, text, &addr4) == 1 ||
	       inet_pton(AF_INET6, text, &addr6) == 1;
}

bool
os_addr_parse(os_addr_t* addr, const char* text, u16 port)
{
	struct in_addr	addr4;
	struct in6_addr	addr6;

	memset(addr, 0, sizeof(*addr));
	if (inet_pton(AF_INET, text, &addr4) == 1)
	{
		struct sockaddr_in* a = (struct sockaddr_in*)addr->raw;

		a->sin_family = AF_INET;
		a->sin_port   = htons(port);
		a->sin_addr   = addr4;
		addr->len     = sizeof(*a);
		return true;
	}
	if (inet_pton(AF_INET6, text, &addr6) == 1)
	{
		struct sockaddr_in6* a = (struct sockaddr_in6*)addr->raw;

		a->sin6_family = AF_INET6;
		a->sin6_port   = htons(port);
		a->sin6_addr   = addr6;
		addr->len      = sizeof(*a);
		return true;
	}
	return false;
}

u32
os_resolve(const char* host, u16 port, bool datagram, os_addr_t* out, u32 cap)
{
	struct addrinfo	hints = { .ai_family   = AF_UNSPEC,
				  .ai_socktype = datagram ? SOCK_DGRAM : SOCK_STREAM,
				  .ai_flags    = AI_NUMERICSERV };
	struct addrinfo*	list = NULL;
	char			service[8];
	u32			n = 0;

	WSADATA		wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return 0;
	snprintf(service, sizeof(service), "%u", port);
	if (getaddrinfo(host, service, &hints, &list) != 0)
		return 0;
	for (const struct addrinfo* ai = list; ai && n < cap; ai = ai->ai_next)
	{
		if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6) ||
		    ai->ai_addrlen > sizeof(out[n].raw))
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		memcpy(out[n].raw, ai->ai_addr, ai->ai_addrlen);
		out[n].len = (u32)ai->ai_addrlen;
		n++;
	}
	freeaddrinfo(list);
	return n;
}

bool
os_addr_is_ipv6(const os_addr_t* addr)
{
	return family_of(addr) == AF_INET6;
}

void
os_addr_text(const os_addr_t* addr, char* out, u32 size)
{
	char ip[INET6_ADDRSTRLEN];

	if (family_of(addr) == AF_INET6)
	{
		const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr->raw;

		inet_ntop(AF_INET6, (void*)&a->sin6_addr, ip, sizeof(ip));
		snprintf(out, size, "[%s]:%u", ip, ntohs(a->sin6_port));
	}
	else
	{
		const struct sockaddr_in* a = (const struct sockaddr_in*)addr->raw;

		inet_ntop(AF_INET, (void*)&a->sin_addr, ip, sizeof(ip));
		snprintf(out, size, "%s:%u", ip, ntohs(a->sin_port));
	}
}

os_socket_t
os_tcp_listen(const os_addr_t* addr, int backlog)
{
	os_socket_t	s   = open_socket(family_of(addr), SOCK_STREAM);
	int		yes = 1;

	if (!os_socket_valid(s))
		return s;
	/* SO_REUSEADDR on Windows lets another process steal the port; ask for the opposite. */
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof(yes));
	if (family_of(addr) == AF_INET6)
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&yes, sizeof(yes));
	if (bind(s, (const struct sockaddr*)addr->raw, (int)addr->len) != 0 ||
	    listen(s, backlog) != 0)
		return fail_close(s);
	return s;
}

os_socket_t
os_tcp_accept(os_socket_t listener, os_addr_t* peer)
{
	int	len = (int)sizeof(peer->raw);
	SOCKET	s   = accept(listener, (struct sockaddr*)peer->raw, &len);

	peer->len = (u32)len;
	if (s == INVALID_SOCKET)
		return OS_SOCKET_INVALID;
	if (!prepare(s))
		return fail_close(s);
	return s;
}

bool
os_tcp_nodelay(os_socket_t s)
{
	int yes = 1;

	return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes)) == 0;
}

os_socket_t
os_udp_bind(const os_addr_t* addr)
{
	os_socket_t	s   = open_socket(family_of(addr), SOCK_DGRAM);
	int		yes = 1;

	if (!os_socket_valid(s))
		return s;
	if (family_of(addr) == AF_INET6)
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&yes, sizeof(yes));
	if (bind(s, (const struct sockaddr*)addr->raw, (int)addr->len) != 0)
		return fail_close(s);
	return s;
}

os_socket_t
os_socket_open(const os_addr_t* addr, bool datagram)
{
	return open_socket(family_of(addr), datagram ? SOCK_DGRAM : SOCK_STREAM);
}

int
os_connect(os_socket_t s, const os_addr_t* addr)
{
	if (connect(s, (const struct sockaddr*)addr->raw, (int)addr->len) == 0)
		return 0;
	return WSAGetLastError() == WSAEWOULDBLOCK ? 1 : -1;
}

int
os_connect_status(os_socket_t s)
{
	int	err = 0;
	int	len = (int)sizeof(err);

	if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0)
		return WSAGetLastError();
	return err;
}

s64
os_socket_recv(os_socket_t s, void* buf, u32 len)
{
	int got = recv(s, (char*)buf, (int)len, 0);

	return got < 0 ? -1 : got;
}

s64
os_socket_send(os_socket_t s, const void* buf, u32 len)
{
	int sent = send(s, (const char*)buf, (int)len, 0);

	return sent < 0 ? -1 : sent;
}

s64
os_udp_recv(os_socket_t s, void* buf, u32 cap, os_addr_t* peer)
{
	int	len = (int)sizeof(peer->raw);
	int	got = recvfrom(s, (char*)buf, (int)cap, 0, (struct sockaddr*)peer->raw, &len);

	peer->len = (u32)len;
	/* A datagram larger than cap is cut and reported as an error without its real size. */
	if (got < 0 && WSAGetLastError() == WSAEMSGSIZE)
		return (s64)cap + 1;
	return got < 0 ? -1 : got;
}

s64
os_udp_send(os_socket_t s, const void* buf, u32 len, const os_addr_t* peer)
{
	int sent = sendto(s, (const char*)buf, (int)len, 0,
			  (const struct sockaddr*)peer->raw, (int)peer->len);

	return sent < 0 ? -1 : sent;
}

void
os_socket_close(os_socket_t s)
{
	int saved = WSAGetLastError();

	closesocket(s);
	WSASetLastError(saved);
}
