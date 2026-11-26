#define _GNU_SOURCE
#include "net/rtu_server.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "net/rtu_framer.h"
#include "os/os_serial.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <string.h>

/**
 * @file
 * @brief Modbus RTU server on a serial-like byte stream.
 *
 * PTY/pipe fds have no baud rate or UART character timing, so true Modbus RTU t3.5/t1.5 cannot
 * be observed from read() boundaries alone. When baud is known (config or tty), derive t3.5 per Modbus spec. Optional strict_t15 enables
 * a best-effort intra-frame t1.5 discard heuristic on read() boundaries in baud-derived auto
 * mode. Otherwise fall back to a fixed conservative silence gap. frame_gap_ms remains an
 * explicit inter-frame override for compatibility and deterministic testing.
 */

static void
rtu_server_init_ctx(rtu_server_t* server)
{
	server->ctx = (rtu_framer_ctx_t){
		.devices = server->options.devices,
		.device_count = server->options.device_count,
		.rng = &server->rng,
		.frame_gap_ms = server->options.frame_gap_ms,
		.baud = server->options.baud,
		.strict_t15 = server->options.strict_t15,
		.broadcast_execute = server->options.broadcast_execute,
		.peer = NULL,
		.obs = server->options.obs,
	};
}

static bool
read_available(rtu_server_t* server, u64 now_ms)
{
	s64	got;
	u64	now_us;
	rtu_framer_t*	f = &server->framer;

	if (server->read_closed)
		return false;
	if (f->rx_len >= sizeof(f->rx))
		rtu_framer_consume(f, f->rx_len - (MODBUS_RTU_MAX_FRAME - 1));

	now_us = clock_now_us();
	rtu_framer_note_gap(f, &server->ctx, now_ms, now_us);

	got = os_serial_read(server->options.fd, f->rx + f->rx_len, sizeof(f->rx) - f->rx_len);
	if (got == 0)
	{
		server->read_closed = true;
		return false;
	}
	if (got < 0)
	{
		if (os_retry())
			return true;
		server->read_closed = true;
		return false;
	}

	f->rx_len += (u32)got;
	f->last_rx_ms = now_ms;
	f->last_rx_us = now_us;
	f->have_rx = true;
	rtu_framer_on_new_data(f, &server->ctx, now_ms);
	return true;
}

static bool
write_pending(rtu_server_t* server)
{
	s64	sent = 0;
	pending_response_t*	p = &server->framer.pending;

	if (!p->active)
		return true;
	if (p->due_ms > clock_now_ms())
		return true;

	sent = os_serial_write(server->options.fd, p->buf + p->sent, p->len - p->sent);
	if (sent < 0)
	{
		if (os_retry())
			return true;
		return false;
	}

	p->sent += (u32)sent;
	if (p->sent >= p->len)
		pending_response_clear(p);
	return true;
}

static void
rtu_build_poll(const rtu_server_t* server, os_wait_entry_t* pfd, int* timeout,
	       u64 now_ms, u32 gap_ms)
{
	u64	due = 0;
	const rtu_framer_t* f = &server->framer;

	*timeout = 1000;
	*pfd = (os_wait_entry_t){ .fd = server->options.fd, .events = 0 };
	if (f->pending.active)
	{
		if (f->pending.due_ms <= now_ms)
		{
			pfd->events |= OS_WAIT_OUT;
			*timeout = 0;
		}
		else
			*timeout = (int)(f->pending.due_ms - now_ms);
		return;
	}

	if (server->read_closed)
	{
		*timeout = 0;
		return;
	}

	pfd->events |= OS_WAIT_IN;
	if (!f->have_rx || f->rx_len == 0)
		return;

	due = f->last_rx_ms + gap_ms;
	if (due > now_ms)
		*timeout = MIN(*timeout, (int)(due - now_ms));
	else
		*timeout = 0;
}

static bool
rtu_handle_hangup(rtu_server_t* server, u64 now_ms)
{
	rtu_framer_t* f = &server->framer;

	if (f->pending.active)
		return net_handle_hangup(&f->pending);
	if (server->read_closed)
		return false;

	server->read_closed = true;
	if (f->have_rx && f->rx_len > 0)
	{
		rtu_framer_process(f, &server->ctx, now_ms);
		if (f->pending.active)
			return net_handle_hangup(&f->pending);
	}
	return false;
}

static bool
rtu_handle_read_closed(rtu_server_t* server, u64 now_ms)
{
	rtu_framer_t* f = &server->framer;

	if (f->pending.active)
		return true;
	if (f->have_rx && f->rx_len > 0)
	{
		rtu_framer_process(f, &server->ctx, now_ms);
		if (f->pending.active)
			return true;
	}
	return false;
}

static bool
rtu_handle_revents(rtu_server_t* server, u32 revents, u64 now_ms)
{
	if (revents & (OS_WAIT_ERR | OS_WAIT_INVAL))
		return false;
	if ((revents & OS_WAIT_OUT) && !write_pending(server))
		return false;
	if (revents & OS_WAIT_IN)
		(void)read_available(server, now_ms);
	if (revents & OS_WAIT_HUP)
		return rtu_handle_hangup(server, now_ms);
	if (server->read_closed)
		return rtu_handle_read_closed(server, now_ms);
	return true;
}

static void
rtu_log_startup(const rtu_server_options_t* options, u32 gap_ms)
{
	if (options->frame_gap_ms > 0)
		LOG_INFO("RTU listening on fd %d (%u device%s), frame gap %u ms (override). Ctrl-C to stop.",
			 options->fd, options->device_count, options->device_count == 1 ? "" : "s", gap_ms);
	else if (options->baud > 0)
		LOG_INFO("RTU listening on fd %d (%u device%s), baud %u, t3.5 %u us (frame gap %u ms). Ctrl-C to stop.",
			 options->fd, options->device_count, options->device_count == 1 ? "" : "s",
			 options->baud, rtu_t35_us(options->baud), gap_ms);
	else
		LOG_INFO("RTU listening on fd %d (%u device%s), frame gap %u ms (default; baud unknown). Ctrl-C to stop.",
			 options->fd, options->device_count, options->device_count == 1 ? "" : "s", gap_ms);
}

static void
rtu_process_gap_due(rtu_server_t* server, u64 now_ms)
{
	if (!rtu_framer_gap_due(&server->framer, server->read_closed, now_ms,
	    rtu_effective_gap_ms(server->options.frame_gap_ms, server->options.baud)))
		return;
	rtu_framer_on_gap_due(&server->framer, &server->ctx, now_ms, clock_now_us());
}

s32
rtu_server_step(rtu_server_t* server, s32 max_wait_ms)
{
	os_wait_entry_t	pfd = { 0 };
	int		timeout = 0;
	u32		gap_ms = rtu_effective_gap_ms(server->options.frame_gap_ms, server->options.baud);
	u64		now_ms = clock_now_ms();

	if (server->stop)
		return NET_STEP_DONE;

	rtu_process_gap_due(server, now_ms);
	rtu_build_poll(server, &pfd, &timeout, now_ms, gap_ms);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (net_poll(server->options.obs, &pfd, 1, timeout) < 0)
	{
		if (os_interrupted())
			return NET_STEP_CONTINUE;	/* the caller re-checks stop */
		LOG_WARN("RTU poll failed: %s", os_last_error_text());
		return NET_STEP_ERROR;
	}

	now_ms = clock_now_ms();
	if (!rtu_handle_revents(server, pfd.revents, now_ms))
		return NET_STEP_DONE;
	rtu_process_gap_due(server, now_ms);
	return NET_STEP_CONTINUE;
}

void
rtu_server_stop(rtu_server_t* server)
{
	server->stop = 1;
}

s32
rtu_server_init(rtu_server_t* server, const rtu_server_options_t* options)
{
	memset(server, 0, sizeof(*server));
	server->options = *options;
	rng_seed(&server->rng, options->seed);
	rtu_framer_init(&server->framer);
	rtu_server_init_ctx(server);
	rtu_log_startup(options, rtu_effective_gap_ms(options->frame_gap_ms, options->baud));
	server->open = true;
	return 0;
}

void
rtu_server_close(rtu_server_t* server)
{
	if (!server->open)
		return;
	server->open = false;
	LOG_INFO("RTU stopped");
}

s32
rtu_server_run(rtu_server_t* server, const rtu_server_options_t* options)
{
	s32 rc = rtu_server_init(server, options);

	if (rc != 0)
		return rc;
	while ((rc = rtu_server_step(server, -1)) == NET_STEP_CONTINUE)
		;
	rtu_server_close(server);
	return rc == NET_STEP_ERROR ? 1 : 0;
}
