#define _GNU_SOURCE
#include "net/ascii_tcp_server.h"
#include "modbus/ascii.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "net/net_tcp_pool.h"
#include "os/os_time.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <string.h>

/**
 * @file
 * @brief Raw Modbus ASCII over TCP byte streams.
 *
 * Framing is CR LF, not silence timing, so a partial frame is expired only after one second
 * without a new byte. Each connection owns its rx buffer and delayed response; devices are
 * borrowed.
 */

static void
conn_close(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn)
{
	net_tcp_pool_close_conn(&server->pool, &conn->base);
}

static void
conn_on_close(void* ctx, net_tcp_conn_t* base)
{
	ascii_tcp_conn_t* conn = (ascii_tcp_conn_t*)base;

	(void)ctx;
	memset(conn, 0, sizeof(*conn));
}

static bool
conn_skip_idle(void* ctx, const net_tcp_conn_t* base)
{
	const ascii_tcp_conn_t* conn = (const ascii_tcp_conn_t*)base;

	(void)ctx;
	return conn->pending.active;
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
	u8			traced[MODBUS_ASCII_MAX_FRAME];

	len = net_frame_rx(server->options.obs, conn->base.peer, frame, len, traced, sizeof(traced));
	if (len == 0)
		return;
	frame = traced;
	if (!modbus_ascii_decode(frame, len, decoded, &addr, &pdu, &pdu_len))
	{
		net_record_comm_error(server->options.obs, server->options.devices, server->options.device_count);
		return;
	}
	dev = addr ? net_find_device(server->options.devices, server->options.device_count, addr, true) : NULL;
	if (!dev)
	{
		/* broadcast or unknown unit: no response, no request processing; never reaches a device */
		net_obs_request(server->options.obs, conn->base.peer, NULL, addr, pdu[0], NULL, 0, NULL);
		return;
	}

	resp_len = net_handle_pdu(server->options.obs, conn->base.peer, dev, addr, &server->rng, now_ms, pdu, pdu_len,
				  resp, &decision);
	if (decision.disconnect)
	{
		conn_close(server, conn);
		return;
	}
	if (decision.drop)
		return;

	frame_len = modbus_ascii_encode(conn->pending.buf, addr, resp, resp_len);
	if (frame_len == 0)
		return;
	/* A checksum fault only damages the emitted LRC; the PDU stays intact. */
	if (decision.corrupt == FAULT_CORRUPT_CHECKSUM)
		modbus_ascii_corrupt_lrc(conn->pending.buf, frame_len);
	frame_len = net_frame_trace(server->options.obs, NET_FRAME_SENT, conn->base.peer, conn->pending.buf,
				    frame_len, MODBUS_ASCII_MAX_FRAME);
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
			net_record_comm_error(server->options.obs, server->options.devices, server->options.device_count);
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
			net_record_comm_error(server->options.obs, server->options.devices, server->options.device_count);
		conn_consume(conn, len);
	}
}

static void
conn_read(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn, u64 now_ms)
{
	s64 got;

	if (conn->read_closed)
		return;
	if (conn->rx_len == sizeof(conn->rx))
		conn->rx_len = 0; /* cannot resync a full buffer without a colon */

	got = os_socket_recv(conn->base.fd, conn->rx + conn->rx_len, sizeof(conn->rx) - conn->rx_len);
	if (got == 0)
	{
		conn->read_closed = true;
		conn->base.last_activity_ms = now_ms;
		return;
	}
	if (got < 0)
	{
		if (os_retry())
			return;
		conn_close(server, conn);
		return;
	}
	conn->rx_len += (u32)got;
	conn->last_rx_ms = now_ms;
	conn->base.last_activity_ms = now_ms;
}

static void
conn_write(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn)
{
	pending_response_t*	p = &conn->pending;
	s64			sent;

	if (!p->active || p->due_ms > os_now_ms())
		return;
	sent = os_socket_send(conn->base.fd, p->buf + p->sent, p->len - p->sent);
	if (sent < 0)
	{
		if (os_retry())
			return;
		conn_close(server, conn);
		return;
	}
	p->sent += (u32)sent;
	conn->base.last_activity_ms = os_now_ms();
	if (p->sent >= p->len)
	{
		pending_response_clear(p);
		if (conn->read_closed)
			conn_close(server, conn);
	}
}

static u32
build_poll_table(ascii_tcp_server_t* server, os_wait_entry_t* fds, net_tcp_conn_t** owners,
		 u64 now_ms, int* timeout)
{
	u32 count = net_tcp_pool_build_poll(&server->pool, fds, owners, now_ms, timeout);

	for (u32 i = 1; i < count; i++)
	{
		ascii_tcp_conn_t* conn = (ascii_tcp_conn_t*)owners[i];

		if (conn->pending.active)
		{
			pending_response_t* p = &conn->pending;

			if (p->due_ms <= now_ms)
			{
				fds[i].events = OS_WAIT_OUT;
				*timeout = 0;
			}
			else
			{
				fds[i].events = 0;
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
		conn_close(server, conn);
		return;
	}
	if (!conn->read_closed)
	{
		conn->read_closed = true;
		conn_process(server, conn, now_ms);
		if (conn->pending.active)
		{
			pending_response_clear(&conn->pending);
			conn_close(server, conn);
			return;
		}
	}
	conn_close(server, conn);
}

static void
conn_handle_revents(ascii_tcp_server_t* server, ascii_tcp_conn_t* conn, u32 revents, u64 now_ms)
{
	if (!os_socket_valid(conn->base.fd))
		return;
	if (revents & (OS_WAIT_ERR | OS_WAIT_INVAL))
	{
		conn_close(server, conn);
		return;
	}
	conn_process(server, conn, now_ms); /* expire stale bytes before adding new ones */
	if (!os_socket_valid(conn->base.fd))
		return;
	if (revents & OS_WAIT_IN)
		conn_read(server, conn, now_ms);
	if (!os_socket_valid(conn->base.fd))
		return;
	conn_process(server, conn, now_ms);
	if (os_socket_valid(conn->base.fd) && (revents & OS_WAIT_OUT))
		conn_write(server, conn);
	if (os_socket_valid(conn->base.fd) && (revents & OS_WAIT_HUP))
	{
		conn_handle_hangup(server, conn, now_ms);
		return;
	}
	if (os_socket_valid(conn->base.fd) &&
	    conn->read_closed &&
	    !conn->pending.active)
		conn_close(server, conn);
}

s32
ascii_tcp_server_step(ascii_tcp_server_t* server, s32 max_wait_ms)
{
	os_wait_entry_t	fds[ASCII_TCP_MAX_CONNECTIONS + 1];
	net_tcp_conn_t*	owners[ASCII_TCP_MAX_CONNECTIONS + 1];
	u32		count;
	int		timeout;
	u64		now_ms = os_now_ms();

	if (net_stop_requested(server->options.obs, server->stop))
		return NET_STEP_DONE;

	net_tcp_pool_close_idle(&server->pool, now_ms);
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		if (os_socket_valid(server->conns[i].base.fd))
			conn_process(server, &server->conns[i], now_ms);
	}

	count = build_poll_table(server, fds, owners, now_ms, &timeout);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (net_poll(server->options.obs, fds, count, timeout) < 0)
	{
		if (os_interrupted() ||
		    net_poll_gathering(server->options.obs))
			return NET_STEP_CONTINUE;	/* the caller re-checks stop */
		LOG_WARN("ASCII-over-TCP poll failed: %s", os_last_error_text());
		return NET_STEP_ERROR;
	}
	now_ms = os_now_ms();
	net_tcp_pool_close_idle(&server->pool, now_ms);
	for (u32 i = 0; i < count; i++)
	{
		ascii_tcp_conn_t* conn = (ascii_tcp_conn_t*)owners[i];
		u32 revents = fds[i].revents;

		if (revents == 0)
			continue;
		if (!conn)
		{
			net_tcp_pool_accept(&server->pool);
			continue;
		}
		conn_handle_revents(server, conn, revents, now_ms);
	}
	return NET_STEP_CONTINUE;
}

void
ascii_tcp_server_stop(ascii_tcp_server_t* server)
{
	server->stop = 1;
}

s32
ascii_tcp_server_init(ascii_tcp_server_t* server, const ascii_tcp_server_options_t* options)
{
	memset(server, 0, sizeof(*server));
	server->options = *options;
	server->pool.conns = (net_tcp_conn_t*)server->conns;
	server->pool.conn_count = ARRAY_COUNT(server->conns);
	server->pool.conn_size = sizeof(server->conns[0]);
	server->pool.idle_timeout_ms = options->idle_timeout_ms;
	server->pool.listen_fd = OS_SOCKET_INVALID;
	server->pool.ctx = server;
	server->pool.obs = options->obs;
	server->pool.on_close = conn_on_close;
	server->pool.skip_idle = conn_skip_idle;
	rng_seed(&server->rng, options->seed);
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
		server->conns[i].base.fd = OS_SOCKET_INVALID;

	if (net_tcp_pool_listen(&server->pool, options->bind_address, options->port) < 0)
		return 2;
	LOG_INFO("ASCII-over-TCP listening on %s:%u (%u device%s). Ctrl-C to stop.",
		 options->bind_address, options->port, options->device_count,
		 options->device_count == 1 ? "" : "s");
	return 0;
}

void
ascii_tcp_server_close(ascii_tcp_server_t* server)
{
	bool was_listening = os_socket_valid(server->pool.listen_fd);

	net_tcp_pool_close_all(&server->pool);
	if (was_listening)
		LOG_INFO("ASCII-over-TCP stopped");
}

s32
ascii_tcp_server_run(ascii_tcp_server_t* server, const ascii_tcp_server_options_t* options)
{
	s32 rc = ascii_tcp_server_init(server, options);

	if (rc != 0)
		return rc;
	while ((rc = ascii_tcp_server_step(server, -1)) == NET_STEP_CONTINUE)
		;
	ascii_tcp_server_close(server);
	return rc == NET_STEP_ERROR ? 1 : 0;
}
