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
 * known (config/CLI or tty), derive t3.5 per Modbus spec and enforce t1.5 as
 * an intra-frame gap on read() boundaries (best-effort on byte streams).
 * Otherwise fall back to a fixed conservative silence gap. frame_gap_ms
 * remains an explicit inter-frame override for compatibility and deterministic
 * testing. */

typedef struct rtu_server_t rtu_server_t;
struct rtu_server_t
{
	rtu_server_options_t	options;
	rng_t			rng;
	rtu_framer_t		framer;
	bool			read_closed;
};

static volatile sig_atomic_t	g_rtu_stop = 0;

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
	rtu_framer_note_gap(f, server->options.devices, server->options.device_count,
			    &server->rng, now_ms, now_us, server->options.frame_gap_ms,
			    server->options.baud, server->options.strict_t15, NULL);

	got = read(server->options.fd, f->rx + f->rx_len, sizeof(f->rx) - f->rx_len);
	if (got == 0)
	{
		server->read_closed = true;
		return false;
	}
	if (got < 0)
	{
		if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
			return true;
		server->read_closed = true;
		return false;
	}

	f->rx_len += (u32)got;
	f->last_rx_ms = now_ms;
	f->last_rx_us = now_us;
	f->have_rx = true;
	f->gap_deferred = false;
	return true;
}

static bool
write_pending(rtu_server_t* server)
{
	ssize_t	sent = 0;
	rtu_pending_t*	p = &server->framer.pending;

	if (!p->active)
		return true;
	if (p->due_ms > clock_now_ms())
		return true;

	sent = write(server->options.fd, p->buf + p->sent, p->len - p->sent);
	if (sent < 0)
	{
		if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
			return true;
		return false;
	}

	p->sent += (u32)sent;
	if (p->sent >= p->len)
		rtu_pending_clear(p);
	return true;
}

static u32
rtu_char_gap_us(u32 baud, u32 fixed_us, u64 char_times_x10)
{
	u64	us;

	if (baud == 0)
		return 0;
	if (baud > RTU_BAUD_FIXED_THRESHOLD)
		return fixed_us;

	/* char time = 11 bits / baud; t1.5/t3.5 are char_times_x10 / 10. */
	us = (char_times_x10 * 11ULL * 1000000ULL + (u64)baud * 10ULL - 1ULL) / ((u64)baud * 10ULL);
	if (us > 0xFFFFFFFFULL)
		return 0xFFFFFFFFU;
	return (u32)us;
}

u32
rtu_t35_us(u32 baud)
{
	return rtu_char_gap_us(baud, RTU_INTERFRAME_GAP_US_HIGH, 35ULL);
}

u32
rtu_t15_us(u32 baud)
{
	return rtu_char_gap_us(baud, RTU_INTERCHAR_GAP_US_HIGH, 15ULL);
}

u32
rtu_effective_gap_ms(u32 frame_gap_ms, u32 baud)
{
	u32	t35;

	if (frame_gap_ms > 0)
		return frame_gap_ms;

	t35 = rtu_t35_us(baud);
	if (t35 > 0)
		return (t35 + 999U) / 1000U; /* ceil to whole ms for poll scheduling */

	return RTU_DEFAULT_FRAME_GAP_MS;
}

bool
rtu_gap_elapsed(u64 now_ms, u64 last_rx_ms, u32 gap_ms)
{
	/* A clock sample taken before last_rx_ms must not wrap into a huge
	 * elapsed time; it simply means no gap has been observed yet. */
	return now_ms >= last_rx_ms && now_ms - last_rx_ms >= gap_ms;
}

static bool
rtu_gap_due(rtu_server_t* server, u64 now_ms, u32 gap_ms)
{
	rtu_framer_t* f = &server->framer;
	return !f->pending.active && f->have_rx && f->rx_len > 0 &&
	       (server->read_closed || rtu_gap_elapsed(now_ms, f->last_rx_ms, gap_ms));
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
rtu_handle_revents(rtu_server_t* server, short revents, u64 now_ms)
{
	if (revents & (POLLERR | POLLNVAL))
		return false;

	if ((revents & POLLOUT) && !write_pending(server))
		return false;

	if (revents & POLLIN)
	{
		/* EOF is reported as POLLIN; read_available() records read_closed and
		 * returns false. Fall through so pending responses can be delivered
		 * before the server exits (issue #32). */
		(void)read_available(server, now_ms);
	}

	if (revents & POLLHUP)
	{
		/* Peer hangup: any already-pending response cannot be delivered. If we
		 * have not seen EOF/read-close yet, process buffered complete frames once
		 * (issue #32), but if that queues a response, drop it and exit instead of
		 * spinning on repeated POLLHUP (issue #40). */
		rtu_framer_t* f = &server->framer;
		if (f->pending.active)
		{
			rtu_pending_clear(&f->pending);
			return false;
		}
		if (!server->read_closed)
		{
			server->read_closed = true;
			if (f->have_rx && f->rx_len > 0)
			{
				rtu_framer_process(f, server->options.devices, server->options.device_count,
						   &server->rng, now_ms, server->options.frame_gap_ms,
						   server->options.baud, NULL);
				if (f->pending.active)
				{
					rtu_pending_clear(&f->pending);
					return false;
				}
			}
		}
		return false;
	}

	if (server->read_closed)
	{
		rtu_framer_t* f = &server->framer;
		if (f->pending.active)
			return true;
		if (f->have_rx && f->rx_len > 0)
		{
			rtu_framer_process(f, server->options.devices, server->options.device_count,
					   &server->rng, now_ms, server->options.frame_gap_ms,
					   server->options.baud, NULL);
			if (f->pending.active)
				return true;
		}
		return false;
	}

	return true;
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
	g_rtu_stop = 0;
	net_install_stop_handlers(&g_rtu_stop);

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

	while (!g_rtu_stop)
	{
		struct pollfd	pfd = { 0 };
		int		timeout = 0;
		u64		now_ms = clock_now_ms();

		if (rtu_gap_due(&server, now_ms, gap_ms))
		{
			rtu_framer_t* f = &server.framer;
			bool incomplete = rtu_framer_process(f, server.options.devices, server.options.device_count,
							     &server.rng, now_ms, server.options.frame_gap_ms,
							     server.options.baud, NULL);
			f->have_rx = f->rx_len > 0;
			if (incomplete && !f->gap_deferred)
			{
				f->gap_deferred = true;
				f->last_rx_ms = now_ms;
			}
			else if (incomplete)
			{
				f->rx_len = 0;
				f->have_rx = false;
				f->gap_deferred = false;
			}
			else
			{
				f->gap_deferred = false;
				continue;
			}
		}

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

		if (rtu_gap_due(&server, now_ms, gap_ms))
		{
			rtu_framer_t* f = &server.framer;
			bool incomplete = rtu_framer_process(f, server.options.devices, server.options.device_count,
							     &server.rng, now_ms, server.options.frame_gap_ms,
							     server.options.baud, NULL);
			f->have_rx = f->rx_len > 0;
			if (incomplete && !f->gap_deferred)
			{
				f->gap_deferred = true;
				f->last_rx_ms = now_ms;
			}
			else if (incomplete)
			{
				f->rx_len = 0;
				f->have_rx = false;
				f->gap_deferred = false;
			}
			else
				f->gap_deferred = false;
		}
	}

	LOG_INFO("RTU stopped");
	return 0;
}
