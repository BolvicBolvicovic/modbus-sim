#define _GNU_SOURCE
#include "net/rtu_server.h"
#include "modbus/rtu.h"
#include "net/net_common.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* PTY/pipe fds have no baud rate, so true Modbus RTU t3.5 is unknown.
 * Use a conservative fixed inter-frame silence as a practical approximation.
 * Configurable via options->frame_gap_ms; default is 20 ms. */
#define RTU_DEFAULT_FRAME_GAP_MS 20

typedef struct rtu_pending_t rtu_pending_t;
struct rtu_pending_t
{
	bool	active;
	u8	buf[MODBUS_RTU_MAX_FRAME];
	u32	len;
	u32	sent;
	u64	due_ms;
};

typedef struct rtu_server_t rtu_server_t;
struct rtu_server_t
{
	rtu_server_options_t	options;
	rng_t			rng;
	u8			rx[MODBUS_RTU_MAX_FRAME * 4];
	u32			rx_len;
	u64			last_rx_ms;
	bool			have_rx;
	bool			read_closed;
	rtu_pending_t		pending;
};

static volatile sig_atomic_t	g_rtu_stop = 0;

static void
pending_clear(rtu_pending_t* pending)
{
	pending->active = false;
	pending->len    = 0;
	pending->sent   = 0;
	pending->due_ms = 0;
}

static void
queue_response(rtu_server_t* server, u8 addr, const u8* pdu, u32 pdu_len, u64 due_ms)
{
	u32 len = modbus_rtu_encode(server->pending.buf, addr, pdu, pdu_len);

	if (len == 0)
		return;

	server->pending.active = true;
	server->pending.len    = len;
	server->pending.sent   = 0;
	server->pending.due_ms = due_ms;
}

static void
handle_frame(rtu_server_t* server, const u8* frame, u32 len, u64 now_ms)
{
	u8			addr    = 0;
	const u8*		pdu     = NULL;
	u32			pdu_len = 0;
	u8			resp[MODBUS_MAX_PDU];
	u32			resp_len = 0;
	device_t*		dev     = NULL;
	fault_decision_t	decision = { 0 };

	if (!modbus_rtu_decode(frame, len, &addr, &pdu, &pdu_len))
		return;

	/* Broadcast (0) intentionally ignored: no broadcast semantics in current
	 * simulator model; RTU response behavior for broadcast is not modeled. */
	if (addr == 0)
		return;

	dev = net_find_device(server->options.devices, server->options.device_count, addr);
	if (!dev)
		return; /* Unknown RTU unit: no response. */

	resp_len = net_handle_pdu(dev, &server->rng, now_ms, pdu, pdu_len, resp, &decision);

	LOG_DEBUG("rtu unit=%u fc=0x%02X -> %s%s", addr, pdu[0],
		  (resp[0] & 0x80) ? "exception" : "ok",
		  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");

	if (decision.drop)
		return;

	queue_response(server, addr, resp, resp_len, now_ms + decision.delay_ms);
}

static void
consume_rx(rtu_server_t* server, u32 n)
{
	if (n >= server->rx_len)
	{
		server->rx_len = 0;
		return;
	}
	memmove(server->rx, server->rx + n, server->rx_len - n);
	server->rx_len -= n;
}

static void
process_rx_frames(rtu_server_t* server, u64 now_ms)
{
	/* After a frame gap, parse complete RTU request frames. Prefer the expected
	 * request length derived from the function code to avoid false CRC matches on
	 * a shorter prefix (issue #18). For variable/unknown FCs, fall back to
	 * decoding the full buffered frame as one frame. */
	while (server->rx_len >= 4 && !server->pending.active)
	{
		u8		addr = 0;
		const u8*	pdu = NULL;
		u32		pdu_len = 0;
		u32		frame_len = 0;
		bool		found = false;

		if (modbus_rtu_request_len(server->rx, server->rx_len, &frame_len))
		{
			if (frame_len <= server->rx_len &&
			    modbus_rtu_decode(server->rx, frame_len, &addr, &pdu, &pdu_len))
			{ found = true; }
			else if (frame_len > server->rx_len)
			{
				if (server->rx_len <= MODBUS_RTU_MAX_FRAME &&
				    modbus_rtu_decode(server->rx, server->rx_len, &addr, &pdu, &pdu_len))
				{
					frame_len = server->rx_len;
					found = true;
				}
				else
				{
					server->rx_len = 0;
					return;
				}
			}
		}

		if (!found && server->rx_len <= MODBUS_RTU_MAX_FRAME &&
		    modbus_rtu_decode(server->rx, server->rx_len, &addr, &pdu, &pdu_len))
		{
			frame_len = server->rx_len;
			found = true;
		}

		if (found)
		{
			handle_frame(server, server->rx, frame_len, now_ms);
			consume_rx(server, frame_len);
			if (server->pending.active)
				return;
			continue;
		}

		/* No valid frame starting at offset 0. On a frame gap, remaining bytes
		 * are noise/partial frames; discard them to avoid false CRC matches. */
		server->rx_len = 0;
		return;
	}

	/* 1-3 bytes cannot form a valid RTU frame (addr+FC+CRC). After the gap
	 * they are stray noise/truncated fragments; drop them so the main loop
	 * can return to poll() instead of spinning. */
	if (server->rx_len > 0 && server->rx_len < 4 && !server->pending.active)
		server->rx_len = 0;

	if (server->rx_len > MODBUS_RTU_MAX_FRAME)
		consume_rx(server, server->rx_len - MODBUS_RTU_MAX_FRAME);
}

static bool
read_available(rtu_server_t* server, u64 now_ms)
{
	ssize_t	got;

	if (server->read_closed)
		return false;
	if (server->rx_len >= sizeof(server->rx))
		consume_rx(server, server->rx_len - (MODBUS_RTU_MAX_FRAME - 1));

	got = read(server->options.fd, server->rx + server->rx_len,
		   sizeof(server->rx) - server->rx_len);
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

	server->rx_len    += (u32)got;
	server->last_rx_ms = now_ms;
	server->have_rx    = true;
	return true;
}

static bool
write_pending(rtu_server_t* server)
{
	ssize_t	sent = 0;

	if (!server->pending.active)
		return true;
	if (server->pending.due_ms > clock_now_ms())
		return true;

	sent = write(server->options.fd, server->pending.buf + server->pending.sent,
		     server->pending.len - server->pending.sent);
	if (sent < 0)
	{
		if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
			return true;
		return false;
	}

	server->pending.sent += (u32)sent;
	if (server->pending.sent >= server->pending.len)
		pending_clear(&server->pending);
	return true;
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
	return !server->pending.active && server->have_rx && server->rx_len > 0 &&
	       (server->read_closed || rtu_gap_elapsed(now_ms, server->last_rx_ms, gap_ms));
}

static void
rtu_build_poll(const rtu_server_t* server, struct pollfd* pfd, int* timeout,
	       u64 now_ms, u32 gap_ms)
{
	u64	due = 0;

	*timeout = 1000;
	*pfd = (struct pollfd){ .fd = server->options.fd, .events = 0 };
	if (server->pending.active)
	{
		if (server->pending.due_ms <= now_ms)
		{
			pfd->events |= POLLOUT;
			*timeout = 0;
		}
		else
			*timeout = (int)(server->pending.due_ms - now_ms);
		return;
	}

	if (server->read_closed)
	{
		*timeout = 0;
		return;
	}

	pfd->events |= POLLIN;
	if (!server->have_rx || server->rx_len == 0)
		return;

	due = server->last_rx_ms + gap_ms;
	if (due > now_ms)
		*timeout = MIN(*timeout, (int)(due - now_ms));
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

	if ((revents & POLLHUP) && !server->read_closed)
	{
		/* Peer hangup before EOF read. Mirror EOF handling: process buffered
		 * frames and let a queued response drain. If a delayed response is
		 * already pending, the peer cannot receive it. */
		if (server->pending.active)
		{
			pending_clear(&server->pending);
			return false;
		}
		server->read_closed = true;
	}

	if (server->read_closed)
	{
		if (server->pending.active)
			return true;
		if (server->have_rx && server->rx_len > 0)
		{
			process_rx_frames(server, now_ms);
			if (server->pending.active)
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
	u32		gap_ms = options->frame_gap_ms ? options->frame_gap_ms : RTU_DEFAULT_FRAME_GAP_MS;

	memset(&server, 0, sizeof(server));
	server.options = *options;
	rng_seed(&server.rng, options->seed);
	pending_clear(&server.pending);
	g_rtu_stop = 0;
	net_install_stop_handlers(&g_rtu_stop);

	LOG_INFO("RTU listening on fd %d (%u device%s), frame gap %u ms. Ctrl-C to stop.",
		 options->fd, options->device_count, options->device_count == 1 ? "" : "s", gap_ms);

	while (!g_rtu_stop)
	{
		struct pollfd	pfd = { 0 };
		int		timeout = 0;
		u64		now_ms = clock_now_ms();

		if (rtu_gap_due(&server, now_ms, gap_ms))
		{
			process_rx_frames(&server, now_ms);
			server.have_rx = server.rx_len > 0;
			continue;
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
			process_rx_frames(&server, now_ms);
			server.have_rx = server.rx_len > 0;
		}
	}

	LOG_INFO("RTU stopped");
	return 0;
}
