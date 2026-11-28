/**
 * @file
 * @brief Shared UDP socket layer: datagram receive, send and delayed replies.
 */
#define _GNU_SOURCE
#include "net/net_udp.h"
#include "net/net_common.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <string.h>

int
net_udp_open(net_udp_t* udp, const char* bind_address, u16 port)
{
	os_addr_t	addrs[NET_BIND_ADDR_MAX];
	u32		n = net_bind_addrs(bind_address, port, true, addrs);

	memset(udp, 0, sizeof(*udp));
	udp->fd = OS_SOCKET_INVALID;
	for (u32 i = 0; i < n && udp->fd == OS_SOCKET_INVALID; i++)
		udp->fd = os_udp_bind(&addrs[i]);
	if (n != 0 &&
	    udp->fd == OS_SOCKET_INVALID)
		LOG_WARN("cannot bind UDP %s:%u: %s", bind_address, port, os_last_error_text());
	return udp->fd == OS_SOCKET_INVALID ? -1 : 0;
}

void
net_udp_close(net_udp_t* udp)
{
	if (os_socket_valid(udp->fd))
		os_socket_close(udp->fd);
	udp->fd = OS_SOCKET_INVALID;
	for (u32 i = 0; i < ARRAY_COUNT(udp->pending); i++)
		udp->pending[i].active = false;
}

s32
net_udp_recv(net_udp_t* udp, u8* buf, u32 cap, net_udp_peer_t* peer)
{
	/* The return value is the real datagram size even when it did not fit. */
	s64 got = os_udp_recv(udp->fd, buf, cap, &peer->addr);

	if (got < 0)
		return os_interrupted() || os_conn_refused() ? -1 : 0;
	os_addr_text(&peer->addr, peer->text, sizeof(peer->text));
	if (got == 0)
	{
		LOG_DEBUG("UDP %s sent an empty datagram, dropped", peer->text);
		return -1;
	}
	if ((u32)got > cap)
	{
		LOG_DEBUG("UDP %s sent a %lld byte datagram (max %u), dropped", peer->text, (long long)got, cap);
		return -1;
	}
	return (s32)got;
}

void
net_udp_send(net_udp_t* udp, const net_udp_peer_t* peer, const u8* buf, u32 len)
{
	if (os_udp_send(udp->fd, buf, len, &peer->addr) < 0)
		LOG_DEBUG("UDP send to %s failed: %s", peer->text, os_last_error_text());
}

bool
net_udp_send_at(net_udp_t* udp, const net_udp_peer_t* peer, const u8* buf, u32 len,
		u64 due_ms, u64 now_ms)
{
	if (due_ms <= now_ms)
	{
		net_udp_send(udp, peer, buf, len);
		return true;
	}
	if (len > NET_UDP_MAX_DATAGRAM)
		return false;
	for (u32 i = 0; i < ARRAY_COUNT(udp->pending); i++)
	{
		struct net_udp_pending_t* slot = &udp->pending[i];

		if (slot->active)
			continue;
		slot->active = true;
		slot->peer   = *peer;
		slot->len    = len;
		slot->due_ms = due_ms;
		memcpy(slot->buf, buf, len);
		return true;
	}
	LOG_WARN("UDP %s: delayed-response table full, response dropped", peer->text);
	return false;
}

void
net_udp_flush_due(net_udp_t* udp, u64 now_ms)
{
	for (u32 i = 0; i < ARRAY_COUNT(udp->pending); i++)
	{
		struct net_udp_pending_t* slot = &udp->pending[i];

		if (!slot->active || slot->due_ms > now_ms)
			continue;
		slot->active = false;
		net_udp_send(udp, &slot->peer, slot->buf, slot->len);
	}
}

void
net_udp_poll_timeout(const net_udp_t* udp, u64 now_ms, int* timeout_ms)
{
	for (u32 i = 0; i < ARRAY_COUNT(udp->pending); i++)
	{
		const struct net_udp_pending_t* slot = &udp->pending[i];

		if (!slot->active)
			continue;
		*timeout_ms = slot->due_ms <= now_ms ? 0 : MIN(*timeout_ms, (int)(slot->due_ms - now_ms));
	}
}
