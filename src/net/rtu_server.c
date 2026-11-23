#define _GNU_SOURCE
#include "net/rtu_server.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/net_common.h"
#include "net/rtu_framer.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* PTY/pipe fds have no baud rate or UART character timing, so true Modbus
 * RTU t3.5/t1.5 cannot be observed from read() boundaries alone. When baud is
 * known (config or tty), derive t3.5 per Modbus spec. Optional strict_t15
 * enables a best-effort intra-frame t1.5 discard heuristic on read()
 * boundaries in baud-derived auto mode. Otherwise fall back to a fixed
 * conservative silence gap. frame_gap_ms remains an explicit inter-frame
 * override for compatibility and deterministic testing. */

typedef struct rtu_server_t rtu_server_t;
struct rtu_server_t
{
	rtu_server_options_t	options;
	rtu_framer_ctx_t	ctx;
	rng_t			rng;
	rtu_framer_t		framer;
	bool			read_closed;
};

static volatile sig_atomic_t	g_rtu_stop = 0;

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
	};
}

static bool
rtu_gap_due(rtu_server_t* server, u64 now_ms, u32 gap_ms)
{
	rtu_framer_t* f = &server->framer;
	return !f->pending.active && f->have_rx && f->rx_len > 0 &&
	       (server->read_closed || rtu_gap_elapsed(now_ms, f->last_rx_ms, gap_ms));
}

static bool
read_available(rtu_server_t* server, u64 now_ms)
{
	ssize_t	got;
	u64	now_us;
	rtu_framer_t*	f = &server->framer;

	if (server->read_closed)
		return false;
	if (f->rx_len >= sizeof(f->rx))
		rtu_framer_consume(f, f->rx_len - (MODBUS_RTU_MAX_FRAME - 1));

	now_us = clock_now_us();
	rtu_framer_note_gap(f, &server->ctx, now_ms, now_us);

	got = read(server->options.fd, f->rx + f->rx_len, sizeof(f->rx) - f->rx_len);
	if (got == 0)
	{
		server->read_closed = true;
		return false;
	}
	if (got < 0)
	{
		if (errno == EAGAIN ||
		    errno == EINTR ||
		    errno == EWOULDBLOCK)
			return true;
		server->read_closed = true;
		return false;
	}

	f->rx_len += (u32)got;
	f->last_rx_ms = now_ms;
	f->last_rx_us = now_us;
	f->have_rx = true;
	if (server->options.frame_gap_ms == 0 &&
	    server->options.baud != 0 &&
	    f->gap_deferred &&
	    f->rx_len > f->gap_deferred_len)
	{
		bool incomplete = rtu_framer_process(f, &server->ctx, now_ms);
		f->have_rx = f->rx_len > 0;
		if (!incomplete)
		{
			f->gap_deferred = false;
			f->gap_deferred_len = 0;
		}
	}
	return true;
}

static bool
write_pending(rtu_server_t* server)
{
	ssize_t	sent = 0;
	pending_response_t*	p = &server->framer.pending;

	if (!p->active)
		return true;
	if (p->due_ms > clock_now_ms())
		return true;

	sent = write(server->options.fd, p->buf + p->sent, p->len - p->sent);
	if (sent < 0)
	{
		if (errno == EAGAIN ||
		    errno == EINTR ||
		    errno == EWOULDBLOCK)
			return true;
		return false;
	}

	p->sent += (u32)sent;
	if (p->sent >= p->len)
		pending_response_clear(p);
	return true;
}

static void
rtu_build_poll(const rtu_server_t* server, struct pollfd* pfd, int* timeout,
	       u64 now_ms, u32 gap_ms)
{
	u64	due = 0;
	const rtu_framer_t* f = &server->framer;

	*timeout = 1000;
	*pfd = (struct pollfd){ .fd = server->options.fd, .events = 0 };
	if (f->pending.active)
	{
		if (f->pending.due_ms <= now_ms)
		{
			pfd->events |= POLLOUT;
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

	pfd->events |= POLLIN;
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
rtu_handle_revents(rtu_server_t* server, short revents, u64 now_ms)
{
	if (revents & (POLLERR | POLLNVAL))
		return false;
	if ((revents & POLLOUT) && !write_pending(server))
		return false;
	if (revents & POLLIN)
		(void)read_available(server, now_ms);
	if (revents & POLLHUP)
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
	if (!rtu_gap_due(server, now_ms, rtu_effective_gap_ms(server->options.frame_gap_ms, server->options.baud)))
		return;
	rtu_framer_on_gap_due(&server->framer, &server->ctx, now_ms, clock_now_us());
}

s32
rtu_server_run(const rtu_server_options_t* options)
{
	rtu_server_t	server;
	u32		gap_ms = rtu_effective_gap_ms(options->frame_gap_ms, options->baud);

	memset(&server, 0, sizeof(server));
	server.options = *options;
	rng_seed(&server.rng, options->seed);
	rtu_framer_init(&server.framer);
	rtu_server_init_ctx(&server);
	g_rtu_stop = 0;
	net_install_stop_handlers(&g_rtu_stop);
	rtu_log_startup(options, gap_ms);

	while (!g_rtu_stop)
	{
		struct pollfd	pfd = { 0 };
		int		timeout = 0;
		u64		now_ms = clock_now_ms();

		rtu_process_gap_due(&server, now_ms);
		rtu_build_poll(&server, &pfd, &timeout, now_ms, gap_ms);
		if (poll(&pfd, 1, timeout) < 0)
		{
			if (errno == EINTR)
				continue;
			LOG_WARN("RTU poll failed: %s", strerror(errno));
			return 1;
		}

		now_ms = clock_now_ms();
		if (!rtu_handle_revents(&server, pfd.revents, now_ms))
			break;
		rtu_process_gap_due(&server, now_ms);
	}

	LOG_INFO("RTU stopped");
	return 0;
}
