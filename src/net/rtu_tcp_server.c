/**
 * @file
 * @brief Modbus RTU frames over TCP connections.
 */
#define _GNU_SOURCE
#include "net/rtu_tcp_server.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "net/net_tcp_pool.h"
#include "net/rtu_framer.h"
#include "os/os_time.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <string.h>

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
		.obs = server->options.obs,
	};
}

static void
conn_close(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn)
{
	net_tcp_pool_close_conn(&server->pool, &conn->base);
}

static void
conn_on_close(void* ctx, net_tcp_conn_t* base)
{
	rtu_tcp_conn_t* conn = (rtu_tcp_conn_t*)base;

	(void)ctx;
	conn->read_closed = false;
	rtu_framer_init(&conn->framer);
}

static void
conn_on_accept(void* ctx, net_tcp_conn_t* base, u64 now_ms)
{
	rtu_tcp_conn_t* conn = (rtu_tcp_conn_t*)base;

	(void)ctx;
	(void)now_ms;
	rtu_framer_init(&conn->framer);
}

static bool
conn_check_fault_disconnect(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn)
{
	if (!os_socket_valid(conn->base.fd) || !conn->framer.fault_disconnect)
		return false;
	conn_close(server, conn);
	return true;
}

static bool
conn_skip_idle(void* ctx, const net_tcp_conn_t* base)
{
	const rtu_tcp_conn_t* conn = (const rtu_tcp_conn_t*)base;

	(void)ctx;
	return conn->framer.pending.active;
}

static void
conn_process_gap_due(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, u64 now_ms, u64 now_us)
{
	rtu_framer_ctx_t ctx = server->ctx;
	ctx.peer = conn->base.peer;
	rtu_framer_on_gap_due(&conn->framer, &ctx, now_ms, now_us);
	(void)conn_check_fault_disconnect(server, conn);
}

static void
conn_process(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, u64 now_ms)
{
	rtu_framer_ctx_t ctx = server->ctx;
	ctx.peer = conn->base.peer;
	rtu_framer_process(&conn->framer, &ctx, now_ms);
	(void)conn_check_fault_disconnect(server, conn);
}

static void
conn_read(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn)
{
	s64 got;
	u64 now_ms = os_now_ms();
	u64 now_us = os_now_us();
	rtu_framer_t* f = &conn->framer;
	rtu_framer_ctx_t ctx = server->ctx;

	ctx.peer = conn->base.peer;
	if (conn->read_closed)
		return;
	if (f->rx_len >= sizeof(f->rx))
		rtu_framer_consume(f, f->rx_len - (MODBUS_RTU_MAX_FRAME - 1));
	rtu_framer_note_gap(f, &ctx, now_ms, now_us);
	if (conn_check_fault_disconnect(server, conn))
		return;

	got = os_socket_recv(conn->base.fd, f->rx + f->rx_len, sizeof(f->rx) - f->rx_len);
	if (got == 0)
	{
		conn->read_closed = true;
		if (f->rx_len > 0)
		{
			rtu_framer_process(f, &ctx, now_ms);
			f->have_rx = f->rx_len > 0;
		}
		if (conn_check_fault_disconnect(server, conn))
			return;
		if (!f->pending.active)
			conn_close(server, conn);
		return;
	}
	if (got < 0 &&
	    !os_retry())
	{
		conn_close(server, conn);
		return;
	}
	if (got < 0)
		return;
	f->rx_len += (u32)got;
	f->last_rx_ms = now_ms;
	f->last_rx_us = now_us;
	f->have_rx = true;
	conn->base.last_activity_ms = now_ms;
	rtu_framer_on_new_data(f, &ctx, now_ms);
	(void)conn_check_fault_disconnect(server, conn);
}

static void
conn_write(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn)
{
	s64 sent;
	pending_response_t* p = &conn->framer.pending;

	if (!p->active)
		return;
	if (p->due_ms > os_now_ms())
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
build_poll_table(rtu_tcp_server_t* server, os_wait_entry_t* fds, net_tcp_conn_t** owners,
		 u64 now_ms, int* timeout)
{
	u32 count = net_tcp_pool_build_poll(&server->pool, fds, owners, now_ms, timeout);

	for (u32 i = 1; i < count; i++)
	{
		rtu_tcp_conn_t* conn = (rtu_tcp_conn_t*)owners[i];

		if (conn->framer.pending.active)
		{
			pending_response_t* p = &conn->framer.pending;

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
		if (conn->framer.have_rx && conn->framer.rx_len > 0)
		{
			u64 due = conn->framer.last_rx_ms + server->gap_ms;

			if (due <= now_ms)
				*timeout = 0;
			else
				*timeout = MIN(*timeout, (int)(due - now_ms));
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
		pending_response_clear(&f->pending);
		conn_close(server, conn);
		return;
	}
	if (!conn->read_closed)
	{
		conn->read_closed = true;
		if (f->have_rx && f->rx_len > 0)
		{
			conn_process(server, conn, os_now_ms());
			if (!os_socket_valid(conn->base.fd))
				return;
			if (f->pending.active)
			{
				pending_response_clear(&f->pending);
				conn_close(server, conn);
				return;
			}
		}
	}
	conn_close(server, conn);
}

static void
conn_handle_revents(rtu_tcp_server_t* server, rtu_tcp_conn_t* conn, u32 revents)
{
	if (!os_socket_valid(conn->base.fd))
		return;
	if (revents & (OS_WAIT_ERR | OS_WAIT_INVAL))
	{
		conn_close(server, conn);
		return;
	}
	if (revents & OS_WAIT_IN)
		conn_read(server, conn);
	if (os_socket_valid(conn->base.fd) && (revents & OS_WAIT_OUT))
		conn_write(server, conn);
	if (os_socket_valid(conn->base.fd) && (revents & OS_WAIT_HUP))
	{
		conn_handle_hangup(server, conn);
		return;
	}
	if (os_socket_valid(conn->base.fd) &&
	    rtu_framer_gap_due(&conn->framer, conn->read_closed, os_now_ms(), server->gap_ms))
		conn_process_gap_due(server, conn, os_now_ms(), os_now_us());
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
rtu_tcp_server_step(rtu_tcp_server_t* server, s32 max_wait_ms)
{
	os_wait_entry_t	fds[RTU_TCP_MAX_CONNECTIONS + 1];
	net_tcp_conn_t*	owners[RTU_TCP_MAX_CONNECTIONS + 1];
	u32		count;
	int		timeout;
	u64		now_ms = os_now_ms();

	if (net_stop_requested(server->options.obs, server->stop))
		return NET_STEP_DONE;

	net_tcp_pool_close_idle(&server->pool, now_ms);
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		rtu_tcp_conn_t* conn = &server->conns[i];

		if (os_socket_valid(conn->base.fd) &&
		    rtu_framer_gap_due(&conn->framer, conn->read_closed, now_ms, server->gap_ms))
			conn_process_gap_due(server, conn, now_ms, os_now_us());
	}

	count = build_poll_table(server, fds, owners, now_ms, &timeout);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (net_poll(server->options.obs, fds, count, timeout) < 0)
		return NET_STEP_CONTINUE;	/* EINTR: the caller re-checks stop */
	now_ms = os_now_ms();
	net_tcp_pool_close_idle(&server->pool, now_ms);
	for (u32 i = 0; i < count; i++)
	{
		rtu_tcp_conn_t* conn = (rtu_tcp_conn_t*)owners[i];
		u32 revents = fds[i].revents;

		if (revents == 0)
			continue;
		if (!conn)
		{
			net_tcp_pool_accept(&server->pool);
			continue;
		}
		conn_handle_revents(server, conn, revents);
	}
	return NET_STEP_CONTINUE;
}

void
rtu_tcp_server_stop(rtu_tcp_server_t* server)
{
	server->stop = 1;
}

s32
rtu_tcp_server_init(rtu_tcp_server_t* server, const rtu_tcp_server_options_t* options)
{
	memset(server, 0, sizeof(*server));
	server->options = *options;
	server->gap_ms = rtu_effective_gap_ms(options->frame_gap_ms, options->baud);
	server->pool.conns = (net_tcp_conn_t*)server->conns;
	server->pool.conn_count = ARRAY_COUNT(server->conns);
	server->pool.conn_size = sizeof(server->conns[0]);
	server->pool.idle_timeout_ms = options->idle_timeout_ms;
	server->pool.listen_fd = OS_SOCKET_INVALID;
	server->pool.ctx = server;
	server->pool.obs = options->obs;
	server->pool.on_accept = conn_on_accept;
	server->pool.on_close = conn_on_close;
	server->pool.skip_idle = conn_skip_idle;
	rng_seed(&server->rng, options->seed);
	rtu_tcp_init_ctx(server);
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		server->conns[i].base.fd = OS_SOCKET_INVALID;
		rtu_framer_init(&server->conns[i].framer);
	}

	if (net_tcp_pool_listen(&server->pool, options->bind_address, options->port) < 0)
		return 2;
	rtu_tcp_log_startup(options, server->gap_ms);
	return 0;
}

void
rtu_tcp_server_close(rtu_tcp_server_t* server)
{
	bool was_listening = os_socket_valid(server->pool.listen_fd);

	net_tcp_pool_close_all(&server->pool);
	if (was_listening)
		LOG_INFO("RTU-over-TCP stopped");
}

s32
rtu_tcp_server_run(rtu_tcp_server_t* server, const rtu_tcp_server_options_t* options)
{
	s32 rc = rtu_tcp_server_init(server, options);

	if (rc != 0)
		return rc;
	while (rtu_tcp_server_step(server, -1) == NET_STEP_CONTINUE)
		;
	rtu_tcp_server_close(server);
	return 0;
}
