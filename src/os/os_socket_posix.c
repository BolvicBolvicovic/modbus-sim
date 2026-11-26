/**
 * @file
 * @brief POSIX sockets and last-error classification.
 */
#define _GNU_SOURCE
#include "os/os_socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

_Static_assert(sizeof(((os_addr_t*)0)->raw) >= sizeof(struct sockaddr_storage),
	       "os_addr_t too small");

static int
family_of(const os_addr_t* addr)
{
	return ((const struct sockaddr*)addr->raw)->sa_family;
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

void
os_addr_text(const os_addr_t* addr, char* out, u32 size)
{
	char ip[INET6_ADDRSTRLEN];

	if (family_of(addr) == AF_INET6)
	{
		const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr->raw;

		inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
		snprintf(out, size, "[%s]:%u", ip, ntohs(a->sin6_port));
	}
	else
	{
		const struct sockaddr_in* a = (const struct sockaddr_in*)addr->raw;

		inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
		snprintf(out, size, "%s:%u", ip, ntohs(a->sin_port));
	}
}

/* Failure paths close the socket without losing the error the caller reports. */
static os_socket_t
fail_close(int s)
{
	int saved = errno;

	close(s);
	errno = saved;
	return OS_SOCKET_INVALID;
}

os_socket_t
os_tcp_listen(const os_addr_t* addr, int backlog)
{
	int s   = socket(family_of(addr), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int yes = 1;

	if (s < 0)
		return OS_SOCKET_INVALID;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (family_of(addr) == AF_INET6)
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
	if (bind(s, (const struct sockaddr*)addr->raw, addr->len) < 0 ||
	    listen(s, backlog) < 0)
		return fail_close(s);
	return s;
}

os_socket_t
os_tcp_accept(os_socket_t listener, os_addr_t* peer)
{
	socklen_t	len = sizeof(peer->raw);
	int		s   = accept4(listener, (struct sockaddr*)peer->raw, &len,
				      SOCK_NONBLOCK | SOCK_CLOEXEC);

	peer->len = len;
	return s < 0 ? OS_SOCKET_INVALID : s;
}

bool
os_tcp_nodelay(os_socket_t s)
{
	int yes = 1;

	return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}

os_socket_t
os_udp_bind(const os_addr_t* addr)
{
	int s   = socket(family_of(addr), SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int yes = 1;

	if (s < 0)
		return OS_SOCKET_INVALID;
	if (family_of(addr) == AF_INET6)
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
	if (bind(s, (const struct sockaddr*)addr->raw, addr->len) < 0)
		return fail_close(s);
	return s;
}

os_socket_t
os_socket_open(const os_addr_t* addr, bool datagram)
{
	int s = socket(family_of(addr),
		       (datagram ? SOCK_DGRAM : SOCK_STREAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	return s < 0 ? OS_SOCKET_INVALID : s;
}

int
os_connect(os_socket_t s, const os_addr_t* addr)
{
	if (connect(s, (const struct sockaddr*)addr->raw, addr->len) == 0)
		return 0;
	return errno == EINPROGRESS ? 1 : -1;
}

int
os_connect_status(os_socket_t s)
{
	int		err = 0;
	socklen_t	len = sizeof(err);

	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
		return errno;
	return err;
}

s64
os_socket_recv(os_socket_t s, void* buf, u32 len)
{
	return recv(s, buf, len, 0);
}

s64
os_socket_send(os_socket_t s, const void* buf, u32 len)
{
	return send(s, buf, len, MSG_NOSIGNAL);
}

s64
os_udp_recv(os_socket_t s, void* buf, u32 cap, os_addr_t* peer)
{
	socklen_t	len = sizeof(peer->raw);
	/* MSG_TRUNC makes the return value the real datagram size even when it did not fit. */
	ssize_t		got = recvfrom(s, buf, cap, MSG_TRUNC, (struct sockaddr*)peer->raw, &len);

	peer->len = len;
	return got;
}

s64
os_udp_send(os_socket_t s, const void* buf, u32 len, const os_addr_t* peer)
{
	return sendto(s, buf, len, MSG_NOSIGNAL, (const struct sockaddr*)peer->raw, peer->len);
}

void
os_socket_close(os_socket_t s)
{
	int saved = errno;

	close(s);
	errno = saved;
}
