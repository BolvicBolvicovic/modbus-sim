/**
 * @file
 * @brief Sockets: addresses, TCP listen/accept/connect, UDP bind, nonblocking I/O and error
 *        classification. Callers never see an OS header or read errno themselves.
 *
 * Every socket is nonblocking and close-on-exec. Failures leave the error for the os_error.h classifiers.
 */
#ifndef OS_SOCKET_H
#define OS_SOCKET_H

#include "os/os_error.h"
#include "vlib/c_types.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct os_addr_t os_addr_t;

#ifdef _WIN32
/** @brief Socket handle (a Winsock SOCKET, which is unsigned). */
typedef uintptr_t os_socket_t;

/** @brief Invalid socket handle; an enum constant cannot hold INVALID_SOCKET. */
#define OS_SOCKET_INVALID ((os_socket_t)~(os_socket_t)0)
#else
/** @brief Socket handle. */
typedef int os_socket_t;

/** @brief Invalid socket handle. */
enum
{
	OS_SOCKET_INVALID = -1,
};
#endif

/** @brief Address text size. */
enum
{
	OS_ADDR_TEXT_SIZE = 64,	/**< fits "[ipv6]:65535" */
};

/** @brief Socket address (IPv4 or IPv6) in an opaque, caller-owned buffer. */
struct os_addr_t
{
	_Alignas(16) u8	raw[128];
	u32		len;
};

/** @brief True for a handle returned by a successful open, listen or accept. */
static inline bool
os_socket_valid(os_socket_t s)
{
	return s != OS_SOCKET_INVALID;
}

/** @brief True for an IPv4 or IPv6 literal. */
bool		os_ip_is_literal(const char* text);

/** @brief True for a plausible host name: letters, digits, '-', '_' and '.' (an IP literal is checked by os_ip_is_literal). */
static inline bool
os_host_name_valid(const char* host)
{
	if (host[0] == '\0')
		return false;
	for (const char* p = host; *p; p++)
	{
		if (!((*p >= 'a' && *p <= 'z') ||
		      (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') ||
		      *p == '-' || *p == '_' || *p == '.'))
			return false;
	}
	return true;
}

/** @brief Parses an IPv4 or IPv6 literal plus port; false when text is not a literal. */
bool		os_addr_parse(os_addr_t* addr, const char* text, u16 port);

/**
 * @brief Resolves a host name or IP literal to at most `cap` addresses (IPv4 and IPv6, in the
 *        resolver's order), for a stream or a datagram socket; returns how many, 0 on failure.
 *
 * Blocks until the resolver answers, which can take as long as its own timeout.
 */
u32		os_resolve(const char* host, u16 port, bool datagram, os_addr_t* out, u32 cap);

/** @brief True when addr is an IPv6 address (else IPv4). */
bool		os_addr_is_ipv6(const os_addr_t* addr);

/** @brief Formats "ip:port", or "[ip]:port" for IPv6. */
void		os_addr_text(const os_addr_t* addr, char* out, u32 size);

/** @brief Opens, binds and listens on a nonblocking TCP socket (IPv6: V6ONLY). */
os_socket_t	os_tcp_listen(const os_addr_t* addr, int backlog);

/** @brief Accepts one connection (nonblocking); OS_SOCKET_INVALID when none or on error. */
os_socket_t	os_tcp_accept(os_socket_t listener, os_addr_t* peer);

/** @brief Disables Nagle's algorithm. */
bool		os_tcp_nodelay(os_socket_t s);

/** @brief Opens and binds a nonblocking UDP socket (IPv6: V6ONLY). */
os_socket_t	os_udp_bind(const os_addr_t* addr);

/** @brief Opens a nonblocking client socket for the family of addr, stream or datagram. */
os_socket_t	os_socket_open(const os_addr_t* addr, bool datagram);

/** @brief Starts connecting: 0 connected, 1 in progress (wait for OUT, then os_connect_status()), -1 failed. */
int		os_connect(os_socket_t s, const os_addr_t* addr);

/** @brief Result of a finished nonblocking connect: 0 on success, else an error code for os_error_text(). */
int		os_connect_status(os_socket_t s);

/** @brief Receives into buf: bytes read, 0 at end of stream, -1 on error (see os_retry()). */
s64		os_socket_recv(os_socket_t s, void* buf, u32 len);

/** @brief Sends without raising a broken-pipe signal: bytes sent, or -1 on error. */
s64		os_socket_send(os_socket_t s, const void* buf, u32 len);

/**
 * @brief Receives one datagram and its sender; returns the datagram's real size even when it
 *        did not fit in cap (truncated), or -1 on error. Where the OS cannot report the real
 *        size of a truncated datagram (Windows), the result is cap + 1.
 */
s64		os_udp_recv(os_socket_t s, void* buf, u32 cap, os_addr_t* peer);

/** @brief Sends one datagram to peer; bytes sent or -1. */
s64		os_udp_send(os_socket_t s, const void* buf, u32 len, const os_addr_t* peer);

/** @brief Closes a socket; the last error is preserved. */
void		os_socket_close(os_socket_t s);

#endif
