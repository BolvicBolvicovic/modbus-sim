/**
 * @file
 * @brief Shared UDP socket layer of the datagram transports.
 */
#ifndef NET_UDP_H
#define NET_UDP_H

#include "modbus/ascii.h"
#include "vlib/c_types.h"
#include <stdbool.h>
#include "vlib/macros.h"
#include <sys/socket.h>

/**
 * @brief Shared datagram socket for the UDP transports (Modbus UDP, and the RTU/ASCII framings
 *        carried in datagrams).
 *
 * One bound nonblocking socket serves every peer; framing, validation and PDU dispatch stay in
 * the servers.
 */
enum
{
	NET_UDP_MAX_DATAGRAM	= MODBUS_ASCII_MAX_FRAME,	/**< largest wire frame of any framing */
	NET_UDP_MAX_PENDING	= 16,	/**< delayed responses queued at once */
	NET_UDP_PEER_TEXT_SIZE	= 64,
	NET_UDP_MAX_BURST	= 32,	/**< datagrams a server should read per step */
};

typedef struct net_udp_peer_t net_udp_peer_t;
struct net_udp_peer_t
{
	struct sockaddr_storage	addr;
	socklen_t		addr_len;
	char			text[NET_UDP_PEER_TEXT_SIZE];	/**< "ip:port" or "[ip]:port" for logs */
};

typedef struct net_udp_t net_udp_t;
struct net_udp_t
{
	int	fd;	/**< -1 when closed */
	struct net_udp_pending_t
	{
		bool		active;
		net_udp_peer_t	peer;
		u8		buf[NET_UDP_MAX_DATAGRAM];
		u32		len;
		u64		due_ms;
	} pending[NET_UDP_MAX_PENDING];
};

/**
 * @brief Resets *udp and binds the socket (IPv4 or IPv6 literal; IPv6 sets V6ONLY).
 *
 * Returns 0, or -1 after a WARN.
 */
int	net_udp_open(net_udp_t* udp, const char* bind_address, u16 port);

/**
 * @brief Closes the socket and forgets queued responses.
 *
 * Idempotent.
 */
void	net_udp_close(net_udp_t* udp);

/**
 * @brief Reads one datagram of at most `cap` bytes.
 *
 * Returns its length (> 0), 0 when nothing is waiting, or -1 when a datagram was consumed and
 * dropped (empty or longer than `cap`; logged at DEBUG), in which case the caller keeps
 * reading. A caller reads up to NET_UDP_MAX_BURST datagrams per step.
 */
s32	net_udp_recv(net_udp_t* udp, u8* buf, u32 cap, net_udp_peer_t* peer);

/** @brief Sends now (best effort; UDP gives no delivery guarantee). */
void	net_udp_send(net_udp_t* udp, const net_udp_peer_t* peer, const u8* buf, u32 len);

/**
 * @brief Sends at due_ms (immediately if already due) from the pending table.
 *
 * Returns false and drops the response when the table is full.
 */
bool	net_udp_send_at(net_udp_t* udp, const net_udp_peer_t* peer, const u8* buf, u32 len,
			u64 due_ms, u64 now_ms);

/** @brief Sends every pending response whose deadline has passed. */
void	net_udp_flush_due(net_udp_t* udp, u64 now_ms);

/** @brief Lowers *timeout_ms (a poll timeout) to the earliest pending deadline. */
void	net_udp_poll_timeout(const net_udp_t* udp, u64 now_ms, int* timeout_ms);

#endif
