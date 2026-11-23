#define _GNU_SOURCE
#include "net/rtu_tcp_server.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/net_common.h"
#include "net/rtu_server.h"
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

typedef struct rtu_tcp_pending_t rtu_tcp_pending_t;
struct rtu_tcp_pending_t
{
	bool	active;
	u8	buf[MODBUS_RTU_MAX_FRAME];
	u32	len;
	u32	sent;
	u64	due_ms;
};

typedef struct rtu_tcp_conn_t rtu_tcp_conn_t;
struct rtu_tcp_conn_t
{
	int			fd;
	u8			rx[MODBUS_RTU_MAX_FRAME * 4];
	u32			rx_len;
	u64			last_rx_ms;
	u64			last_rx_us;
	bool			have_rx;
	rtu_tcp_pending_t	pending;
	u64			last_activity_ms;
	char			peer[INET_ADDRSTRLEN + 8];
};

typedef struct rtu_tcp_server_t rtu_tcp_server_t;
struct rtu_tcp_server_t
{
	rtu_tcp_server_options_t	options;
	int				listen_fd;
	u64				accept_backoff_until_ms;
	bool				accept_backoff_logged;
	rng_t				rng;
	rtu_tcp_conn_t			conns[RTU_TCP_MAX_CONNECTIONS];
};

static volatile sig_atomic_t	g_rtu_tcp_stop = 0;

static void
pending_clear(rtu_tcp_pending_t* pending)
{
	pending->active = false;
	pending->len    = 0;
	pending->sent   = 0;
	pending->due_ms = 0;
}

static void
conn_close(rtu_tcp_conn_t* conn)
{
	LOG_DEBUG("client %s disconnected", conn->peer);
	close(conn->fd);
	conn->fd = -1;
	conn->rx_len = 0;
	conn->have_rx = false;
	pending_clear(&conn->pending);
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
	struct sockaddr_in	addr = { 0 };
	int			yes  = 1;
	int			fd   = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(server->options.port);
	if (inet_pton(AF_INET, server->options.bind_address, &addr.sin_addr) != 1)
	{
		LOG_WARN("bind address '%s' is not an IPv4 address", server->options.bind_address);
		close(fd);
		return -1;
	}
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0)
	{
		LOG_WARN("cannot listen on %s:%u: %s", server->options.bind_address,
			 server->options.port, strerror(errno));
		close(fd);
		return -1;
	}
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
		pending_clear(&conn->pending);
		snprintf(conn->peer, sizeof(conn->peer), "%s:%u", ip, ntohs(addr.sin_port));
		LOG_DEBUG("client %s connected", conn->peer);
	}
}

static void
consume_rx(rtu_tcp_conn_t* conn, u32 n)
{
	if (n >= conn->rx_len)
	{
		conn->rx_len = 0;
		return;
	}
	memmove(conn->rx, conn->rx + n, conn->rx_len - n);
	conn->rx_len -= n;
}

static bool
queue_response(rtu_tcp_conn_t* conn, u8 addr, const u8* pdu, u32 pdu_len, u64 due_ms)
{
	u32 len = modbus_rtu_encode(conn->pending.buf, addr, pdu, pdu_len);

	if (len == 0)
		return false;
	conn->pending.active = true;
	conn->pending.len = len;
	conn->pending.sent = 0;
	conn->pending.due_ms = due_ms;
	return true;
}

static void
handle_frame(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, const u8* frame, u32 len, u64 now_ms)
{
	u8		addr = 0;
	const u8*	pdu = NULL;
	u32		pdu_len = 0;
	u8		resp[MODBUS_MAX_PDU];
	u32		resp_len = 0;
	device_t*	dev = NULL;
	fault_decision_t decision = { 0 };

	if (!modbus_rtu_decode(frame, len, &addr, &pdu, &pdu_len))
		return;
	if (addr == 0)
		return; /* broadcast: no response */
	dev = net_find_device(server->options.devices, server->options.device_count, addr);
	if (!dev)
		return; /* unknown unit: no response */

	resp_len = net_handle_pdu(dev, &server->rng, now_ms, pdu, pdu_len, resp, &decision);
	LOG_DEBUG("%s rtu unit=%u fc=0x%02X -> %s%s", conn->peer, addr, pdu[0],
		  (resp[0] & 0x80) ? "exception" : "ok",
		  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");
	if (decision.drop)
		return;
	(void)queue_response(conn, addr, resp, resp_len, now_ms + decision.delay_ms);
}

typedef enum rtu_frame_result_t rtu_frame_result_t;
enum rtu_frame_result_t
{
	RTU_FRAME_NONE,
	RTU_FRAME_FOUND,
	RTU_FRAME_DISCARD,
};

static rtu_frame_result_t
rtu_next_frame(const u8* rx, u32 rx_len, u32* frame_len)
{
	u8		addr    = 0;
	const u8*	pdu     = NULL;
	u32		pdu_len = 0;
	u32		expected = 0;

	if (modbus_rtu_request_len(rx, rx_len, &expected))
	{
		if (expected <= rx_len && modbus_rtu_decode(rx, expected, &addr, &pdu, &pdu_len))
		{
			*frame_len = expected;
			return RTU_FRAME_FOUND;
		}
		if (expected > rx_len)
		{
			if (rx_len <= MODBUS_RTU_MAX_FRAME &&
			    modbus_rtu_decode(rx, rx_len, &addr, &pdu, &pdu_len))
			{
				*frame_len = rx_len;
				return RTU_FRAME_FOUND;
			}
			return RTU_FRAME_DISCARD;
		}
	}

	if (rx_len <= MODBUS_RTU_MAX_FRAME &&
	    modbus_rtu_decode(rx, rx_len, &addr, &pdu, &pdu_len))
	{
		*frame_len = rx_len;
		return RTU_FRAME_FOUND;
	}
	return RTU_FRAME_NONE;
}

static void
process_rx_frames(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, u64 now_ms)
{
	while (conn->rx_len >= 4 && !conn->pending.active)
	{
		u32			frame_len = 0;
		rtu_frame_result_t	result = rtu_next_frame(conn->rx, conn->rx_len, &frame_len);

		if (result != RTU_FRAME_FOUND)
		{
			conn->rx_len = 0;
			return;
		}

		handle_frame(server, conn, conn->rx, frame_len, now_ms);
		consume_rx(conn, frame_len);
		if (conn->pending.active)
			return;
	}

	if (conn->rx_len > 0 && conn->rx_len < 4 && !conn->pending.active)
		conn->rx_len = 0;
	if (conn->rx_len > MODBUS_RTU_MAX_FRAME)
		consume_rx(conn, conn->rx_len - MODBUS_RTU_MAX_FRAME);
}

static void
note_rx_gap(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, u64 now_ms, u64 now_us)
{
	u32 t15, t35;

	if (!conn->have_rx || conn->rx_len == 0 || conn->pending.active)
		return;
	if (server->options.frame_gap_ms > 0 || server->options.baud == 0)
		return;
	if (now_us < conn->last_rx_us)
		return;
	t15 = rtu_t15_us(server->options.baud);
	t35 = rtu_t35_us(server->options.baud);
	if (t15 == 0 || t35 == 0)
		return;
	{
		u64 elapsed = now_us - conn->last_rx_us;
		if (elapsed >= t35)
		{
			process_rx_frames(server, conn, now_ms);
			conn->have_rx = conn->rx_len > 0;
			return;
		}
		if (elapsed >= t15)
		{
			conn->rx_len = 0;
			conn->have_rx = false;
		}
	}
}

static void
conn_read(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn)
{
	ssize_t got;
	u64 now_ms = clock_now_ms();
	u64 now_us = clock_now_us();

	if (conn->rx_len >= sizeof(conn->rx))
		consume_rx(conn, conn->rx_len - (MODBUS_RTU_MAX_FRAME - 1));
	note_rx_gap(server, conn, now_ms, now_us);

	got = recv(conn->fd, conn->rx + conn->rx_len, sizeof(conn->rx) - conn->rx_len, 0);
	if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK))
	{
		conn_close(conn);
		return;
	}
	if (got < 0)
		return;
	conn->rx_len += (u32)got;
	conn->last_rx_ms = now_ms;
	conn->last_rx_us = now_us;
	conn->have_rx = true;
	conn->last_activity_ms = now_ms;
}

static void
conn_write(rtu_tcp_conn_t* conn)
{
	ssize_t sent;

	if (!conn->pending.active)
		return;
	if (conn->pending.due_ms > clock_now_ms())
		return;
	sent = send(conn->fd, conn->pending.buf + conn->pending.sent,
		    conn->pending.len - conn->pending.sent, MSG_NOSIGNAL);
	if (sent < 0)
	{
		if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
			return;
		conn_close(conn);
		return;
	}
	conn->pending.sent += (u32)sent;
	conn->last_activity_ms = clock_now_ms();
	if (conn->pending.sent >= conn->pending.len)
		pending_clear(&conn->pending);
}

static bool
gap_due(rtu_tcp_conn_t* conn, u64 now_ms, u32 gap_ms)
{
	return !conn->pending.active && conn->have_rx && conn->rx_len > 0 &&
	       rtu_gap_elapsed(now_ms, conn->last_rx_ms, gap_ms);
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
		if (conn->fd < 0 || conn->pending.active)
			continue;
		deadline = conn->last_activity_ms + idle_ms;
		if (now_ms < deadline)
			continue;
		LOG_INFO("client %s idle for %u ms, closing", conn->peer, idle_ms);
		conn_close(conn);
	}
}

static u32
build_poll_table(rtu_tcp_server_t* server, struct pollfd* fds, rtu_tcp_conn_t** owners, u64 now_ms, int* timeout, u32 gap_ms)
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

		if (conn->pending.active)
		{
			if (conn->pending.due_ms <= now_ms)
			{
				fds[count - 1].events = POLLOUT;
				*timeout = 0;
			}
			else
			{
				fds[count - 1].events = 0;
				*timeout = MIN(*timeout, (int)(conn->pending.due_ms - now_ms));
			}
			continue;
		}

		if (conn->have_rx && conn->rx_len > 0)
		{
			u64 due = conn->last_rx_ms + gap_ms;
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

s32
rtu_tcp_server_run(const rtu_tcp_server_options_t* options)
{
	rtu_tcp_server_t server = { 0 };
	u32 gap_ms = rtu_effective_gap_ms(options->frame_gap_ms, options->baud);

	server.options = *options;
	server.listen_fd = -1;
	rng_seed(&server.rng, options->seed);
	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
	{
		server.conns[i].fd = -1;
		pending_clear(&server.conns[i].pending);
	}

	g_rtu_tcp_stop = 0;
	net_install_stop_handlers(&g_rtu_tcp_stop);
	if (listen_open(&server) < 0)
		return 2;

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

	while (!g_rtu_tcp_stop)
	{
		struct pollfd fds[RTU_TCP_MAX_CONNECTIONS + 1];
		rtu_tcp_conn_t* owners[RTU_TCP_MAX_CONNECTIONS + 1];
		u32 count;
		int timeout;
		u64 now_ms = clock_now_ms();

		close_idle_connections(&server, now_ms);
		for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
		{
			rtu_tcp_conn_t* conn = &server.conns[i];
			if (conn->fd < 0)
				continue;
			if (gap_due(conn, now_ms, gap_ms))
			{
				process_rx_frames(&server, conn, now_ms);
				conn->have_rx = conn->rx_len > 0;
			}
		}

		count = build_poll_table(&server, fds, owners, now_ms, &timeout, gap_ms);
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
			if (conn->fd < 0)
				continue;
			if (revents & (POLLERR | POLLNVAL | POLLHUP))
			{
				conn_close(conn);
				continue;
			}
			if (revents & POLLIN)
				conn_read(&server, conn);
			if (conn->fd >= 0 && (revents & POLLOUT))
				conn_write(conn);
			if (conn->fd >= 0 && gap_due(conn, clock_now_ms(), gap_ms))
			{
				process_rx_frames(&server, conn, clock_now_ms());
				conn->have_rx = conn->rx_len > 0;
			}
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
