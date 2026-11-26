/**
 * @file
 * @brief Shared TCP listener and connection-pool logic of the three TCP transports.
 */
#define _GNU_SOURCE
#include "net/net_tcp_pool.h"
#include "net/net_obs.h"
#include "net/net_common.h"
#include "util/clock.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum
{
	NET_TCP_POLL_TIMEOUT_MS	     = 1000,
	NET_TCP_ACCEPT_BACKOFF_MS    = 1000,
	NET_TCP_LISTEN_BACKLOG	     = 64,
};

static net_tcp_conn_t*
conn_at(net_tcp_pool_t* pool, u32 index)
{
	return (net_tcp_conn_t*)((u8*)pool->conns + (size_t)index * pool->conn_size);
}

static net_tcp_conn_t*
find_free(net_tcp_pool_t* pool)
{
	for (u32 i = 0; i < pool->conn_count; i++)
	{
		net_tcp_conn_t* conn = conn_at(pool, i);

		if (conn->fd < 0)
			return conn;
	}
	return NULL;
}

static void
format_peer(net_tcp_conn_t* conn, const struct sockaddr_storage* addr)
{
	char ip[INET6_ADDRSTRLEN];

	if (addr->ss_family == AF_INET6)
	{
		const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;

		inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
		snprintf(conn->peer, sizeof(conn->peer), "[%s]:%u", ip, ntohs(a->sin6_port));
	}
	else
	{
		const struct sockaddr_in* a = (const struct sockaddr_in*)addr;

		inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
		snprintf(conn->peer, sizeof(conn->peer), "%s:%u", ip, ntohs(a->sin_port));
	}
}

int
net_tcp_pool_listen(net_tcp_pool_t* pool, const char* bind_address, u16 port)
{
	int fd = net_tcp_listen(bind_address, port, NET_TCP_LISTEN_BACKLOG);

	if (fd < 0)
		return -1;
	pool->listen_fd = fd;
	return 0;
}

/* DEBUG line for a client connection; JSON logs get event, transport and peer fields. */
static void
log_connection(const net_tcp_pool_t* pool, const net_tcp_conn_t* conn, const char* event,
	       const char* verb)
{
	char	peer[2 * NET_TCP_PEER_SIZE];
	char	fields[256];

	if (!log_enabled(LOG_LEVEL_DEBUG))
		return;
	log_json_escape(peer, sizeof(peer), conn->peer);
	snprintf(fields, sizeof(fields), "\"event\":\"%s\",\"transport\":\"%s\",\"peer\":\"%s\"", event,
		 pool->obs && pool->obs->transport ? pool->obs->transport : "tcp", peer);
	log_write_fields(LOG_LEVEL_DEBUG, fields, "client %s %s", conn->peer, verb);
}

void
net_tcp_pool_accept(net_tcp_pool_t* pool)
{
	struct sockaddr_storage addr = { 0 };
	socklen_t		len = sizeof(addr);
	u64			now_ms;
	int			fd;
	int			yes = 1;
	net_tcp_conn_t*		conn;

	for (;;)
	{
		len = sizeof(addr);
		fd  = accept4(pool->listen_fd, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK);
		if (fd < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (errno == EMFILE || errno == ENFILE)
			{
				if (!pool->accept_backoff_logged)
				{
					LOG_WARN("accept failed: %s; backing off for %u ms",
						 strerror(errno), NET_TCP_ACCEPT_BACKOFF_MS);
					pool->accept_backoff_logged = true;
				}
				pool->accept_backoff_until_ms = clock_now_ms() + NET_TCP_ACCEPT_BACKOFF_MS;
				return;
			}
			LOG_WARN("accept failed: %s", strerror(errno));
			return;
		}
		pool->accept_backoff_logged = false;
		conn = find_free(pool);
		if (!conn)
		{
			LOG_WARN("too many clients, refusing a new one");
			close(fd);
			continue;
		}
		if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
			LOG_WARN("cannot set TCP_NODELAY on accepted socket: %s", strerror(errno));

		now_ms                 = clock_now_ms();
		memset(conn, 0, pool->conn_size);
		conn->fd               = fd;
		conn->last_activity_ms = now_ms;
		format_peer(conn, &addr);
		log_connection(pool, conn, "connect", "connected");
		net_obs_connection(pool->obs, true);
		if (pool->on_accept)
			pool->on_accept(pool->ctx, conn, now_ms);
	}
}

void
net_tcp_pool_close_conn(net_tcp_pool_t* pool, net_tcp_conn_t* conn)
{
	int fd;

	if (!conn || conn->fd < 0)
		return;
	log_connection(pool, conn, "disconnect", "disconnected");
	net_obs_connection(pool->obs, false);
	fd = conn->fd;
	if (pool->on_close)
		pool->on_close(pool->ctx, conn);
	close(fd);
	conn->fd = -1;
}

void
net_tcp_pool_close_idle(net_tcp_pool_t* pool, u64 now_ms)
{
	u32 idle_ms = pool->idle_timeout_ms;

	if (idle_ms == 0)
		return;
	for (u32 i = 0; i < pool->conn_count; i++)
	{
		net_tcp_conn_t* conn = conn_at(pool, i);
		u64		deadline;

		if (conn->fd < 0)
			continue;
		if (pool->skip_idle && pool->skip_idle(pool->ctx, conn))
			continue;
		deadline = conn->last_activity_ms + idle_ms;
		if (now_ms < deadline)
			continue;
		LOG_INFO("client %s idle for %u ms, closing", conn->peer, idle_ms);
		net_tcp_pool_close_conn(pool, conn);
	}
}

void
net_tcp_pool_close_all(net_tcp_pool_t* pool)
{
	for (u32 i = 0; i < pool->conn_count; i++)
		net_tcp_pool_close_conn(pool, conn_at(pool, i));
	if (pool->listen_fd >= 0)
	{
		close(pool->listen_fd);
		pool->listen_fd = -1;
	}
}

u32
net_tcp_pool_build_poll(net_tcp_pool_t* pool, struct pollfd* fds, net_tcp_conn_t** owners,
			u64 now_ms, int* timeout)
{
	u32	count   = 0;
	u32	idle_ms = pool->idle_timeout_ms;
	bool	backoff = pool->accept_backoff_until_ms > now_ms;

	*timeout = NET_TCP_POLL_TIMEOUT_MS;
	fds[count] = (struct pollfd){ .fd = pool->listen_fd, .events = backoff ? 0 : POLLIN };
	owners[count++] = NULL;
	if (backoff)
		*timeout = MIN(*timeout, (int)(pool->accept_backoff_until_ms - now_ms));

	for (u32 i = 0; i < pool->conn_count; i++)
	{
		net_tcp_conn_t* conn = conn_at(pool, i);
		u64		idle_deadline;

		if (conn->fd < 0)
			continue;
		fds[count] = (struct pollfd){ .fd = conn->fd, .events = POLLIN };
		owners[count++] = conn;
		if (idle_ms == 0 || (pool->skip_idle && pool->skip_idle(pool->ctx, conn)))
			continue;
		idle_deadline = conn->last_activity_ms + idle_ms;
		if (idle_deadline <= now_ms)
			*timeout = 0;
		else
			*timeout = MIN(*timeout, (int)(idle_deadline - now_ms));
	}
	return count;
}
