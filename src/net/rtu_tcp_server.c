#define _GNU_SOURCE
#include "net/rtu_tcp_server.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/net_common.h"
#include "net/rtu_framer.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RTU_TCP_MAX_CONNECTIONS	32
#define RTU_TCP_IDLE_POLL_MS	1000
#define RTU_TCP_ACCEPT_BACKOFF_MS 1000

typedef struct rtu_tcp_conn_t rtu_tcp_conn_t;
struct rtu_tcp_conn_t
{
	int		fd;
	rtu_framer_t	framer;
	bool		read_closed;
	u64		last_activity_ms;
	char		peer[INET_ADDRSTRLEN + 8];
};

typedef struct rtu_tcp_server_t rtu_tcp_server_t;
struct rtu_tcp_server_t
{
	rtu_tcp_server_options_t	options;
	rtu_framer_ctx_t		ctx;
	int				listen_fd;
	u32				gap_ms;
	u64				accept_backoff_until_ms;
	bool				accept_backoff_logged;
	rng_t				rng;
	rtu_tcp_conn_t			conns[RTU_TCP_MAX_CONNECTIONS];
};

static volatile sig_atomic_t	g_rtu_tcp_stop = 0;

static void
rtu_tcp_init_ctx(rtu_tcp_server_t* server)
{
	server->ctx = (rtu_framer_ctx_t){
		.devices = server->options.devices,
		.device_count = server->options.device_count,
		.rng = &server->rng,
		.frame_gap_ms = server->options.frame_gap_ms,
		.baud = server->options.baud,
		.strict_t15 = false,
		.broadcast_execute = server->options.broadcast_execute,
		.peer = NULL,
	};
}

static bool
gap_due(rtu_tcp_conn_t* conn, u64 now_ms, u32 gap_ms)
{
	rtu_framer_t* f = &conn->framer;
	return !f->pending.active && f->have_rx && f->rx_len > 0 &&
	       (conn->read_closed || rtu_gap_elapsed(now_ms, f->last_rx_ms, gap_ms));
}

static void
conn_close(rtu_tcp_conn_t* conn)
{
	LOG_DEBUG("client %s disconnected", conn->peer);
	close(conn->fd);
	conn->fd = -1;
	conn->read_closed = false;
	rtu_framer_init(&conn->framer);
}

static rtu_tcp_conn_t*
conn_find_free(rtu_tcp_server_t* server)
{
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		if (server->conns[i].fd < 0)
			return &server->conns[i];
	}
	return NULL;
}

static s32
listen_open(rtu_tcp_server_t* server)
{
	int fd = net_tcp_listen(server->options.bind_address, server->options.port, 64);
	if (fd < 0)
		return -1;
	server->listen_fd = fd;
	return 0;
}

static void
accept_clients(rtu_tcp_server_t* server)
{
	struct sockaddr_in	addr = { 0 };
	socklen_t		len  = sizeof(addr);
	char			ip[INET_ADDRSTRLEN];
	int			fd, yes = 1;
	rtu_tcp_conn_t*		conn;

	for (;;)
	{
		len = sizeof(addr);
		fd  = accept4(server->listen_fd, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK);
		if (fd < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (errno == EMFILE || errno == ENFILE)
			{
				if (!server->accept_backoff_logged)
				{
					LOG_WARN("accept failed: %s; backing off for %u ms",
						 strerror(errno), RTU_TCP_ACCEPT_BACKOFF_MS);
					server->accept_backoff_logged = true;
				}
				server->accept_backoff_until_ms = clock_now_ms() + RTU_TCP_ACCEPT_BACKOFF_MS;
				return;
			}
			LOG_WARN("accept failed: %s", strerror(errno));
			return;
		}
		server->accept_backoff_logged = false;
		conn = conn_find_free(server);
		if (!conn)
		{
			LOG_WARN("too many clients, refusing a new one");
			close(fd);
			continue;
		}
		if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
			LOG_WARN("cannot set TCP_NODELAY on accepted socket: %s", strerror(errno));
		inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		memset(conn, 0, sizeof(*conn));
		conn->fd = fd;
		conn->last_activity_ms = clock_now_ms();
		rtu_framer_init(&conn->framer);
		snprintf(conn->peer, sizeof(conn->peer), "%s:%u", ip, ntohs(addr.sin_port));
		LOG_DEBUG("client %s connected", conn->peer);
	}
}

static void
conn_process_gap_due(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, u64 now_ms, u64 now_us)
{
	rtu_framer_ctx_t ctx = server->ctx;
	ctx.peer = conn->peer;
	rtu_framer_on_gap_due(&conn->framer, &ctx, now_ms, now_us);
}

static void
conn_process(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, u64 now_ms)
{
	rtu_framer_ctx_t ctx = server->ctx;
	ctx.peer = conn->peer;
	rtu_framer_process(&conn->framer, &ctx, now_ms);
}

static void
conn_read(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn)
{
	ssize_t got;
	u64 now_ms = clock_now_ms();
	u64 now_us = clock_now_us();
	rtu_framer_t* f = &conn->framer;
	rtu_framer_ctx_t ctx = server->ctx;

	ctx.peer = conn->peer;
	if (conn->read_closed)
		return;
	if (f->rx_len >= sizeof(f->rx))
		rtu_framer_consume(f, f->rx_len - (MODBUS_RTU_MAX_FRAME - 1));
	rtu_framer_note_gap(f, &ctx, now_ms, now_us);

	got = recv(conn->fd, f->rx + f->rx_len, sizeof(f->rx) - f->rx_len, 0);
	if (got == 0)
	{
		conn->read_closed = true;
		if (f->rx_len > 0)
		{
			rtu_framer_process(f, &ctx, now_ms);
			f->have_rx = f->rx_len > 0;
		}
		if (!f->pending.active)
			conn_close(conn);
		return;
	}
	if (got < 0 &&
	    errno != EAGAIN &&
	    errno != EINTR &&
	    errno != EWOULDBLOCK)
	{
		conn_close(conn);
		return;
	}
	if (got < 0)
		return;
	f->rx_len += (u32)got;
	f->last_rx_ms = now_ms;
	f->last_rx_us = now_us;
	f->have_rx = true;
	conn->last_activity_ms = now_ms;
	if (server->options.frame_gap_ms == 0 &&
	    server->options.baud != 0 &&
	    f->gap_deferred &&
	    f->rx_len > f->gap_deferred_len)
	{
		bool incomplete = rtu_framer_process(f, &ctx, now_ms);
		f->have_rx = f->rx_len > 0;
		if (!incomplete)
		{
			f->gap_deferred = false;
			f->gap_deferred_len = 0;
		}
	}
}

static void
conn_write(rtu_tcp_conn_t* conn)
{
	ssize_t sent;
	rtu_pending_t* p = &conn->framer.pending;

	if (!p->active)
		return;
	if (p->due_ms > clock_now_ms())
		return;
	sent = send(conn->fd, p->buf + p->sent, p->len - p->sent, MSG_NOSIGNAL);
	if (sent < 0)
	{
		if (errno == EAGAIN ||
		    errno == EINTR ||
		    errno == EWOULDBLOCK)
			return;
		conn_close(conn);
		return;
	}
	p->sent += (u32)sent;
	conn->last_activity_ms = clock_now_ms();
	if (p->sent >= p->len)
	{
		rtu_pending_clear(p);
		if (conn->read_closed)
			conn_close(conn);
	}
}

static void
close_idle_connections(rtu_tcp_server_t* server, u64 now_ms)
{
	u32 idle_ms = server->options.idle_timeout_ms;
	if (idle_ms == 0)
		return;
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		rtu_tcp_conn_t* conn = &server->conns[i];
		u64 deadline;
		if (conn->fd < 0 || conn->framer.pending.active)
			continue;
		deadline = conn->last_activity_ms + idle_ms;
		if (now_ms < deadline)
			continue;
		LOG_INFO("client %s idle for %u ms, closing", conn->peer, idle_ms);
		conn_close(conn);
	}
}

static u32
build_poll_table(rtu_tcp_server_t* server, struct pollfd* fds, rtu_tcp_conn_t** owners, u64 now_ms, int* timeout)
{
	u32 count = 0;
	u32 idle_ms = server->options.idle_timeout_ms;
	bool backoff = server->accept_backoff_until_ms > now_ms;

	*timeout = RTU_TCP_IDLE_POLL_MS;
	fds[count] = (struct pollfd){ .fd = server->listen_fd, .events = backoff ? 0 : POLLIN };
	owners[count++] = NULL;
	if (backoff)
		*timeout = MIN(*timeout, (int)(server->accept_backoff_until_ms - now_ms));

	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		rtu_tcp_conn_t* conn = &server->conns[i];
		if (conn->fd < 0)
			continue;
		fds[count] = (struct pollfd){ .fd = conn->fd, .events = POLLIN };
		owners[count++] = conn;

		if (conn->framer.pending.active)
		{
			rtu_pending_t* p = &conn->framer.pending;
			if (p->due_ms <= now_ms)
			{
				fds[count - 1].events = POLLOUT;
				*timeout = 0;
			}
			else
			{
				fds[count - 1].events = 0;
				*timeout = MIN(*timeout, (int)(p->due_ms - now_ms));
			}
			continue;
		}
		if (conn->read_closed)
		{
			*timeout = 0;
			continue;
		}
		if (conn->framer.have_rx && conn->framer.rx_len > 0)
		{
			u64 due = conn->framer.last_rx_ms + server->gap_ms;
			if (due <= now_ms)
				*timeout = 0;
			else
				*timeout = MIN(*timeout, (int)(due - now_ms));
		}
		if (idle_ms > 0)
		{
			u64 idle_deadline = conn->last_activity_ms + idle_ms;
			if (idle_deadline <= now_ms)
				*timeout = 0;
			else
				*timeout = MIN(*timeout, (int)(idle_deadline - now_ms));
		}
	}
	return count;
}

static void
conn_handle_hangup(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn)
{
	rtu_framer_t* f = &conn->framer;

	if (f->pending.active)
	{
		if (conn->read_closed)
			return;
		rtu_pending_clear(&f->pending);
		conn_close(conn);
		return;
	}
	if (!conn->read_closed)
	{
		conn->read_closed = true;
		if (f->have_rx && f->rx_len > 0)
		{
			conn_process(server, conn, clock_now_ms());
			if (f->pending.active)
			{
				rtu_pending_clear(&f->pending);
				conn_close(conn);
				return;
			}
		}
	}
	conn_close(conn);
}

static void
conn_handle_revents(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, short revents)
{
	if (conn->fd < 0)
		return;
	if (revents & (POLLERR | POLLNVAL))
	{
		conn_close(conn);
		return;
	}
	if (revents & POLLIN)
		conn_read(server, conn);
	if (conn->fd >= 0 && (revents & POLLOUT))
		conn_write(conn);
	if (conn->fd >= 0 && (revents & POLLHUP))
	{
		conn_handle_hangup(server, conn);
		return;
	}
	if (conn->fd >= 0 && gap_due(conn, clock_now_ms(), server->gap_ms))
		conn_process_gap_due(server, conn, clock_now_ms(), clock_now_us());
}

static void
rtu_tcp_log_startup(const rtu_tcp_server_options_t* options, u32 gap_ms)
{
	if (options->frame_gap_ms > 0)
		LOG_INFO("RTU-over-TCP listening on %s:%u (%u device%s), frame gap %u ms (override). Ctrl-C to stop.",
			 options->bind_address, options->port, options->device_count,
			 options->device_count == 1 ? "" : "s", gap_ms);
	else if (options->baud > 0)
		LOG_INFO("RTU-over-TCP listening on %s:%u (%u device%s), baud %u, t3.5 %u us (frame gap %u ms). Ctrl-C to stop.",
			 options->bind_address, options->port, options->device_count,
			 options->device_count == 1 ? "" : "s", options->baud, rtu_t35_us(options->baud), gap_ms);
	else
		LOG_INFO("RTU-over-TCP listening on %s:%u (%u device%s), frame gap %u ms (default; baud unknown). Ctrl-C to stop.",
			 options->bind_address, options->port, options->device_count,
			 options->device_count == 1 ? "" : "s", gap_ms);
}

s32
rtu_tcp_server_run(const rtu_tcp_server_options_t* options)
{
	rtu_tcp_server_t server = { 0 };
	struct pollfd fds[RTU_TCP_MAX_CONNECTIONS + 1];
	rtu_tcp_conn_t* owners[RTU_TCP_MAX_CONNECTIONS + 1];

	server.options = *options;
	server.gap_ms = rtu_effective_gap_ms(options->frame_gap_ms, options->baud);
	server.listen_fd = -1;
	rng_seed(&server.rng, options->seed);
	rtu_tcp_init_ctx(&server);
	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
	{
		server.conns[i].fd = -1;
		rtu_framer_init(&server.conns[i].framer);
	}

	g_rtu_tcp_stop = 0;
	net_install_stop_handlers(&g_rtu_tcp_stop);
	if (listen_open(&server) < 0)
		return 2;
	rtu_tcp_log_startup(options, server.gap_ms);

	while (!g_rtu_tcp_stop)
	{
		u32 count;
		int timeout;
		u64 now_ms = clock_now_ms();

		close_idle_connections(&server, now_ms);
		for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
		{
			rtu_tcp_conn_t* conn = &server.conns[i];
			if (conn->fd >= 0 && gap_due(conn, now_ms, server.gap_ms))
				conn_process_gap_due(&server, conn, now_ms, clock_now_us());
		}

		count = build_poll_table(&server, fds, owners, now_ms, &timeout);
		if (poll(fds, count, timeout) < 0)
			continue;
		now_ms = clock_now_ms();
		close_idle_connections(&server, now_ms);
		for (u32 i = 0; i < count; i++)
		{
			rtu_tcp_conn_t* conn = owners[i];
			short revents = fds[i].revents;
			if (revents == 0)
				continue;
			if (!conn)
			{
				accept_clients(&server);
				continue;
			}
			conn_handle_revents(&server, conn, revents);
		}
	}

	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
	{
		if (server.conns[i].fd >= 0)
			close(server.conns[i].fd);
	}
	if (server.listen_fd >= 0)
		close(server.listen_fd);
	LOG_INFO("RTU-over-TCP stopped");
	return 0;
}
