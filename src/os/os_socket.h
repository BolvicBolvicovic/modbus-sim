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

typedef int os_socket_t;
typedef struct os_addr_t os_addr_t;

enum
{
	OS_SOCKET_INVALID = -1,
	OS_ADDR_TEXT_SIZE = 64,	/**< fits "[ipv6]:65535" */
};

/** @brief Socket address (IPv4 or IPv6) in an opaque, caller-owned buffer. */
struct os_addr_t
{
	_Alignas(16) u8	raw[128];
	u32		len;
};

/** @brief True for an IPv4 or IPv6 literal. */
bool		os_ip_is_literal(const char* text);

/** @brief Parses an IPv4 or IPv6 literal plus port; false when text is not a literal. */
bool		os_addr_parse(os_addr_t* addr, const char* text, u16 port);

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
 *        did not fit in cap (truncated), or -1 on error.
 */
s64		os_udp_recv(os_socket_t s, void* buf, u32 cap, os_addr_t* peer);

/** @brief Sends one datagram to peer; bytes sent or -1. */
s64		os_udp_send(os_socket_t s, const void* buf, u32 len, const os_addr_t* peer);

/** @brief Closes a socket; the last error is preserved. */
void		os_socket_close(os_socket_t s);

#endif
