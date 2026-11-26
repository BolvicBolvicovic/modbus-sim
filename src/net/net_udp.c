#define _GNU_SOURCE
#include "net/net_udp.h"
#include "net/net_common.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
format_peer(net_udp_peer_t* peer)
{
	char ip[INET6_ADDRSTRLEN];

	if (peer->addr.ss_family == AF_INET6)
	{
		const struct sockaddr_in6* a = (const struct sockaddr_in6*)&peer->addr;

		inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
		snprintf(peer->text, sizeof(peer->text), "[%s]:%u", ip, ntohs(a->sin6_port));
	}
	else
	{
		const struct sockaddr_in* a = (const struct sockaddr_in*)&peer->addr;

		inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
		snprintf(peer->text, sizeof(peer->text), "%s:%u", ip, ntohs(a->sin_port));
	}
}

int
net_udp_open(net_udp_t* udp, const char* bind_address, u16 port)
{
	struct sockaddr_storage	addr;
	socklen_t		addr_len = 0;
	int			family = net_bind_addr(bind_address, port, &addr, &addr_len);
	int			yes = 1;
	int			fd;

	memset(udp, 0, sizeof(*udp));
	udp->fd = -1;
	if (family == AF_UNSPEC)
		return -1;

	fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		LOG_WARN("cannot create UDP socket for %s:%u: %s", bind_address, port, strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
	if (bind(fd, (struct sockaddr*)&addr, addr_len) < 0)
	{
		LOG_WARN("cannot bind UDP %s:%u: %s", bind_address, port, strerror(errno));
		close(fd);
		return -1;
	}
	udp->fd = fd;
	return 0;
}

void
net_udp_close(net_udp_t* udp)
{
	if (udp->fd >= 0)
		close(udp->fd);
	udp->fd = -1;
	for (u32 i = 0; i < ARRAY_COUNT(udp->pending); i++)
		udp->pending[i].active = false;
}

s32
net_udp_recv(net_udp_t* udp, u8* buf, u32 cap, net_udp_peer_t* peer)
{
	ssize_t got;

	peer->addr_len = sizeof(peer->addr);
	/* MSG_TRUNC makes the return value the real datagram size even when it did not fit. */
	got = recvfrom(udp->fd, buf, cap, MSG_TRUNC, (struct sockaddr*)&peer->addr, &peer->addr_len);
	if (got < 0)
		return errno == EINTR || errno == ECONNREFUSED ? -1 : 0;
	format_peer(peer);
	if (got == 0)
	{
		LOG_DEBUG("UDP %s sent an empty datagram, dropped", peer->text);
		return -1;
	}
	if ((u32)got > cap)
	{
		LOG_DEBUG("UDP %s sent a %zd byte datagram (max %u), dropped", peer->text, got, cap);
		return -1;
	}
	return (s32)got;
}

void
net_udp_send(net_udp_t* udp, const net_udp_peer_t* peer, const u8* buf, u32 len)
{
	if (sendto(udp->fd, buf, len, MSG_NOSIGNAL, (const struct sockaddr*)&peer->addr,
		   peer->addr_len) < 0)
		LOG_DEBUG("UDP send to %s failed: %s", peer->text, strerror(errno));
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
