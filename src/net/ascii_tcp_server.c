#define _GNU_SOURCE
#include "net/ascii_tcp_server.h"
#include "modbus/ascii.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
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

/* Raw Modbus ASCII over TCP byte streams. Framing is CR LF, not silence timing,
 * so a partial frame is expired only after one second without a new byte. Each
 * connection owns its rx buffer and delayed response; devices are borrowed. */

enum
{
	ASCII_TCP_MAX_CONNECTIONS   = 32,
	ASCII_TCP_IDLE_POLL_MS      = 1000,
	ASCII_TCP_ACCEPT_BACKOFF_MS = 1000,
	ASCII_TCP_INTERCHAR_TIMEOUT_MS = 1000,
};

typedef struct ascii_tcp_conn_t ascii_tcp_conn_t;
struct ascii_tcp_conn_t
{
	int		fd;
	u8		rx[MODBUS_ASCII_MAX_FRAME];
	u32		rx_len;
	u64		last_rx_ms;
	u64		last_activity_ms;
	pending_response_t	pending;
	bool		read_closed;
	char		peer[INET6_ADDRSTRLEN + 8];
};

typedef struct ascii_tcp_server_t ascii_tcp_server_t;
struct ascii_tcp_server_t
{
	ascii_tcp_server_options_t	options;
	int				listen_fd;
	u64				accept_backoff_until_ms;
	bool				accept_backoff_logged;
	rng_t				rng;
	ascii_tcp_conn_t		conns[ASCII_TCP_MAX_CONNECTIONS];
};

static volatile sig_atomic_t	g_ascii_tcp_stop = 0;

static void
conn_close(ascii_tcp_conn_t* conn)
{
	LOG_DEBUG("client %s disconnected", conn->peer);
	close(conn->fd);
	memset(conn, 0, sizeof(*conn));
	conn->fd = -1;
}

static ascii_tcp_conn_t*
conn_find_free(ascii_tcp_server_t* server)
{
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		if (server->conns[i].fd < 0)
			return &server->conns[i];
	}
	return NULL;
}

static s32
listen_open(ascii_tcp_server_t* server)
{
	int fd = net_tcp_listen(server->options.bind_address, server->options.port, 64);

	if (fd < 0)
		return -1;
	server->listen_fd = fd;
	return 0;
}

static void
accept_clients(ascii_tcp_server_t* server)
{
	struct sockaddr_storage	addr = { 0 };
	socklen_t		len  = sizeof(addr);
	char			ip[INET6_ADDRSTRLEN];
	int			fd, yes = 1;
	ascii_tcp_conn_t*	conn;

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
						 strerror(errno), ASCII_TCP_ACCEPT_BACKOFF_MS);
					server->accept_backoff_logged = true;
				}
				server->accept_backoff_until_ms = clock_now_ms() + ASCII_TCP_ACCEPT_BACKOFF_MS;
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
		memset(conn, 0, sizeof(*conn));
		conn->fd = fd;
		conn->last_activity_ms = clock_now_ms();
		if (addr.ss_family == AF_INET6)
		{
			struct sockaddr_in6* a = (struct sockaddr_in6*)&addr;

			inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
			snprintf(conn->peer, sizeof(conn->peer), "[%s]:%u", ip, ntohs(a->sin6_port));
		}
		else
		{
			struct sockaddr_in* a = (struct sockaddr_in*)&addr;

			inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
			snprintf(conn->peer, sizeof(conn->peer), "%s:%u", ip, ntohs(a->sin_port));
		}
		LOG_DEBUG("client %s connected", conn->peer);
	}
}

static void
conn_consume(ascii_tcp_conn_t* conn, u32 n)
{
	if (n >= conn->rx_len)
	{
		conn->rx_len = 0;
		return;
	}
	memmove(conn->rx, conn->rx + n, conn->rx_len - n);
	conn->rx_len -= n;
}

static void
conn_handle_frame(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn, u64 now_ms,
		  const u8* frame, u32 len)
{
	u8			decoded[MODBUS_MAX_PDU + 2];
	u8			addr = 0;
	const u8*		pdu = NULL;
	u32			pdu_len = 0;
	u8			resp[MODBUS_MAX_PDU];
	u32			resp_len = 0;
	u32			frame_len = 0;
	device_t*		dev = NULL;
	fault_decision_t	decision = { 0 };

	if (!modbus_ascii_decode(frame, len, decoded, &addr, &pdu, &pdu_len))
	{
		net_record_comm_error(server->options.devices, server->options.device_count);
		return;
	}
	if (addr == 0)
		return; /* broadcast: no response, no request processing */
	dev = net_find_device(server->options.devices, server->options.device_count, addr);
	if (!dev)
		return; /* unknown unit: no response */

	resp_len = net_handle_pdu(dev, &server->rng, now_ms, pdu, pdu_len, resp, &decision);
	LOG_DEBUG("ascii-tcp client=%s unit=%u fc=0x%02X -> %s%s", conn->peer, addr, pdu[0],
		  (resp[0] & 0x80) ? "exception" : "ok",
		  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");
	if (decision.drop)
		return;

	frame_len = modbus_ascii_encode(conn->pending.buf, addr, resp, resp_len);
	if (frame_len == 0)
		return;
	conn->pending.active = true;
	conn->pending.len = frame_len;
	conn->pending.sent = 0;
	conn->pending.due_ms = now_ms + decision.delay_ms;
}

static void
conn_process(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn, u64 now_ms)
{
	const u8*	rx = NULL;
	u32		remaining = 0;
	bool		have_frame = false;

	/* Inter-frame whitespace is not a malformed frame. Drop it before the
	 * look-ahead scan and the partial-frame timeout below. */
	while (conn->rx_len > 0)
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(conn->rx, conn->rx_len, &len);

		if (scan != MODBUS_ASCII_SCAN_SKIP || len == 0)
			break;
		conn_consume(conn, len);
	}

	rx = conn->rx;
	remaining = conn->rx_len;

	/* Bytes buffered behind a delayed response are not stale just because no
	 * new byte arrived; only a partial frame may be expired. Look past
	 * discardable prefixes so second-colon resync still works there too. */
	while (remaining > 0)
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(rx, remaining, &len);

		if (scan == MODBUS_ASCII_SCAN_FRAME)
		{
			have_frame = true;
			break;
		}
		if (scan != MODBUS_ASCII_SCAN_DISCARD || len == 0)
			break;
		rx += len;
		remaining -= len;
	}

	if (!have_frame &&
	    conn->rx_len > 0 &&
	    conn->last_rx_ms > 0)
	{
		u64 idle_ms = now_ms >= conn->last_rx_ms ?
			      now_ms - conn->last_rx_ms : 0;

		if (idle_ms >= ASCII_TCP_INTERCHAR_TIMEOUT_MS)
		{
			conn->rx_len = 0;
			conn->last_rx_ms = 0;
			net_record_comm_error(server->options.devices, server->options.device_count);
		}
	}

	while (conn->rx_len > 0 && !conn->pending.active)
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(conn->rx, conn->rx_len, &len);

		if (scan == MODBUS_ASCII_SCAN_SKIP)
		{
			conn_consume(conn, len);
			continue;
		}
		if (scan == MODBUS_ASCII_SCAN_NONE || len == 0)
			break;
		if (scan == MODBUS_ASCII_SCAN_FRAME)
			conn_handle_frame(server, conn, now_ms, conn->rx, len);
		else
			net_record_comm_error(server->options.devices, server->options.device_count);
		conn_consume(conn, len);
	}
}

static void
conn_read(ascii_tcp_conn_t* conn, u64 now_ms)
{
	ssize_t got;

	if (conn->read_closed)
		return;
	if (conn->rx_len == sizeof(conn->rx))
		conn->rx_len = 0; /* cannot resync a full buffer without a colon */

	got = recv(conn->fd, conn->rx + conn->rx_len, sizeof(conn->rx) - conn->rx_len, 0);
	if (got == 0)
	{
		conn->read_closed = true;
		conn->last_activity_ms = now_ms;
		return;
	}
	if (got < 0)
	{
		if (errno == EAGAIN ||
		    errno == EINTR ||
		    errno == EWOULDBLOCK)
			return;
		conn_close(conn);
		return;
	}
	conn->rx_len += (u32)got;
	conn->last_rx_ms = now_ms;
	conn->last_activity_ms = now_ms;
}

static void
conn_write(ascii_tcp_conn_t* conn)
{
	pending_response_t*	p = &conn->pending;
	ssize_t			sent;

	if (!p->active || p->due_ms > clock_now_ms())
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
		pending_response_clear(p);
		if (conn->read_closed)
			conn_close(conn);
	}
}

static void
close_idle_connections(ascii_tcp_server_t* server, u64 now_ms)
{
	u32 idle_ms = server->options.idle_timeout_ms;

	if (idle_ms == 0)
		return;
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		ascii_tcp_conn_t* conn = &server->conns[i];
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
build_poll_table(ascii_tcp_server_t* server, struct pollfd* fds, ascii_tcp_conn_t** owners,
		 u64 now_ms, int* timeout)
{
	u32 count = 0;
	u32 idle_ms = server->options.idle_timeout_ms;
	bool backoff = server->accept_backoff_until_ms > now_ms;

	*timeout = ASCII_TCP_IDLE_POLL_MS;
	fds[count] = (struct pollfd){ .fd = server->listen_fd, .events = backoff ? 0 : POLLIN };
	owners[count++] = NULL;
	if (backoff)
		*timeout = MIN(*timeout, (int)(server->accept_backoff_until_ms - now_ms));

	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		ascii_tcp_conn_t* conn = &server->conns[i];

		if (conn->fd < 0)
			continue;
		fds[count] = (struct pollfd){ .fd = conn->fd, .events = POLLIN };
		owners[count++] = conn;

		if (conn->pending.active)
		{
			pending_response_t* p = &conn->pending;

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
		if (conn->rx_len > 0 && conn->last_rx_ms > 0)
		{
			u64 due = conn->last_rx_ms + ASCII_TCP_INTERCHAR_TIMEOUT_MS;

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
conn_handle_hangup(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn, u64 now_ms)
{
	if (conn->pending.active)
	{
		if (conn->read_closed)
			return;
		pending_response_clear(&conn->pending);
		conn_close(conn);
		return;
	}
	if (!conn->read_closed)
	{
		conn->read_closed = true;
		conn_process(server, conn, now_ms);
		if (conn->pending.active)
		{
			pending_response_clear(&conn->pending);
			conn_close(conn);
			return;
		}
	}
	conn_close(conn);
}

static void
conn_handle_revents(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn, short revents, u64 now_ms)
{
	if (conn->fd < 0)
		return;
	if (revents & (POLLERR | POLLNVAL))
	{
		conn_close(conn);
		return;
	}
	conn_process(server, conn, now_ms); /* expire stale bytes before adding new ones */
	if (revents & POLLIN)
		conn_read(conn, now_ms);
	conn_process(server, conn, now_ms);
	if (conn->fd >= 0 && (revents & POLLOUT))
		conn_write(conn);
	if (conn->fd >= 0 && (revents & POLLHUP))
	{
		conn_handle_hangup(server, conn, now_ms);
		return;
	}
	if (conn->fd >= 0 &&
	    conn->read_closed &&
	    !conn->pending.active)
		conn_close(conn);
}

s32
ascii_tcp_server_run(const ascii_tcp_server_options_t* options)
{
	ascii_tcp_server_t	server = { 0 };
	struct pollfd		fds[ASCII_TCP_MAX_CONNECTIONS + 1];
	ascii_tcp_conn_t*	owners[ASCII_TCP_MAX_CONNECTIONS + 1];

	server.options = *options;
	server.listen_fd = -1;
	rng_seed(&server.rng, options->seed);
	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
		server.conns[i].fd = -1;

	g_ascii_tcp_stop = 0;
	net_install_stop_handlers(&g_ascii_tcp_stop);
	if (listen_open(&server) < 0)
		return 2;
	LOG_INFO("ASCII-over-TCP listening on %s:%u (%u device%s). Ctrl-C to stop.",
		 options->bind_address, options->port, options->device_count,
		 options->device_count == 1 ? "" : "s");

	while (!g_ascii_tcp_stop)
	{
		u32 count;
		int timeout;
		u64 now_ms = clock_now_ms();

		close_idle_connections(&server, now_ms);
		for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
		{
			if (server.conns[i].fd >= 0)
				conn_process(&server, &server.conns[i], now_ms);
		}

		count = build_poll_table(&server, fds, owners, now_ms, &timeout);
		if (poll(fds, count, timeout) < 0)
		{
			if (errno == EINTR)
				continue;
			LOG_WARN("ASCII-over-TCP poll failed: %s", strerror(errno));
			return 1;
		}
		now_ms = clock_now_ms();
		close_idle_connections(&server, now_ms);
		for (u32 i = 0; i < count; i++)
		{
			ascii_tcp_conn_t* conn = owners[i];
			short revents = fds[i].revents;

			if (revents == 0)
				continue;
			if (!conn)
			{
				accept_clients(&server);
				continue;
			}
			conn_handle_revents(&server, conn, revents, now_ms);
		}
	}

	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
	{
		if (server.conns[i].fd >= 0)
			close(server.conns[i].fd);
	}
	if (server.listen_fd >= 0)
		close(server.listen_fd);
	LOG_INFO("ASCII-over-TCP stopped");
	return 0;
}
