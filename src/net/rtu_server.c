#define _GNU_SOURCE
#include "net/rtu_server.h"
#include "modbus/rtu.h"
#include "modbus/pdu.h"
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
	rtu_pending_t		pending;
};

static volatile sig_atomic_t	g_rtu_stop = 0;

static void
on_rtu_stop_signal(int signo)
{
	(void)signo;
	g_rtu_stop = 1;
}

static void
install_rtu_signal_handlers(void)
{
	struct sigaction	sa = { 0 };

	sa.sa_handler = on_rtu_stop_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static device_t*
find_device(const rtu_server_t* server, u8 unit)
{
	for (u32 i = 0; i < server->options.device_count; i++)
	{
		if (server->options.devices[i]->unit_id == unit)
			return server->options.devices[i];
	}
	return NULL;
}

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

	dev = find_device(server, addr);
	if (!dev)
		return; /* Unknown RTU unit: no response. */

	resp_len = modbus_handle_pdu(dev, now_ms, pdu, pdu_len, resp);
	decision = fault_decide(&dev->fault, &server->rng);

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

static bool
rtu_request_frame_len(const u8* buf, u32 buf_len, u32* frame_len)
{
	u8	fc;
	u32	pdu_len = 0;

	if (buf_len < 2)
		return false;

	fc = buf[1];
	switch (fc)
	{
	case FC_READ_COILS:
	case FC_READ_DISCRETE_INPUTS:
	case FC_READ_HOLDING_REGISTERS:
	case FC_READ_INPUT_REGISTERS:
	case FC_WRITE_SINGLE_COIL:
	case FC_WRITE_SINGLE_REGISTER:
		pdu_len = 5;
		break;
	case FC_READ_EXCEPTION_STATUS:
	case FC_GET_COM_EVENT_COUNTER:
	case FC_GET_COM_EVENT_LOG:
	case FC_REPORT_SERVER_ID:
		pdu_len = 1;
		break;
	case FC_WRITE_MULTIPLE_COILS:
	case FC_WRITE_MULTIPLE_REGISTERS:
		if (buf_len < 7)
			return false;
		pdu_len = 6 + (u32)buf[6];
		break;
	case FC_MASK_WRITE_REGISTER:
		pdu_len = 7;
		break;
	case FC_READ_WRITE_MULTIPLE_REGISTERS:
		if (buf_len < 11)
			return false;
		pdu_len = 10 + (u32)buf[9];
		break;
	case FC_READ_FIFO_QUEUE:
		pdu_len = 3;
		break;
	case FC_READ_FILE_RECORD:
	case FC_WRITE_FILE_RECORD:
		if (buf_len < 3)
			return false;
		pdu_len = 2 + (u32)buf[2];
		break;
	case FC_ENCAPSULATED_INTERFACE_TRANSPORT:
		if (buf_len < 3)
			return false;
		if (buf[2] == 0x0E) /* MEI type 14: Read Device Identification */
			pdu_len = 4;
		else
			return false;
		break;
	default:
		return false;
	}

	if (pdu_len < 1 || pdu_len > MODBUS_MAX_PDU)
		return false;

	*frame_len = 1 + pdu_len + 2;
	return true;
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

		if (rtu_request_frame_len(server->rx, server->rx_len, &frame_len))
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
read_available(rtu_server_t* server)
{
	ssize_t	got;

	if (server->rx_len >= sizeof(server->rx))
		consume_rx(server, server->rx_len - (MODBUS_RTU_MAX_FRAME - 1));

	got = read(server->options.fd, server->rx + server->rx_len,
		   sizeof(server->rx) - server->rx_len);
	if (got == 0)
		return false;
	if (got < 0)
	{
		if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
			return true;
		return false;
	}

	server->rx_len    += (u32)got;
	server->last_rx_ms = clock_now_ms();
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
	install_rtu_signal_handlers();

	LOG_INFO("RTU listening on fd %d (%u device%s), frame gap %u ms. Ctrl-C to stop.",
		 options->fd, options->device_count, options->device_count == 1 ? "" : "s", gap_ms);

	while (!g_rtu_stop)
	{
		struct pollfd	pfd = { .fd = options->fd, .events = 0 };
		int		timeout = 1000;
		u64		now_ms = clock_now_ms();

		if (server.pending.active)
		{
			if (server.pending.due_ms <= now_ms)
			{
				pfd.events |= POLLOUT;
				timeout = 0;
			}
			else
				timeout = (int)(server.pending.due_ms - now_ms);
		}
		else
		{
			pfd.events |= POLLIN;
			if (server.have_rx && server.rx_len > 0)
			{
				u64 due = server.last_rx_ms + gap_ms;

				if (due <= now_ms)
				{
					process_rx_frames(&server, now_ms);
					server.have_rx = server.rx_len > 0;
					continue;
				}
				timeout = MIN(timeout, (int)(due - now_ms));
			}
		}

		if (poll(&pfd, 1, timeout) < 0)
		{
			if (errno == EINTR)
				continue;
			LOG_WARN("RTU poll failed: %s", strerror(errno));
			return 1;
		}

		now_ms = clock_now_ms();
		if (pfd.revents & (POLLERR | POLLNVAL))
			break;

		if ((pfd.revents & POLLOUT) && !write_pending(&server))
			break;

		if (pfd.revents & POLLIN)
		{
			if (!read_available(&server))
			{
				if (server.have_rx && server.rx_len > 0 && !server.pending.active)
					process_rx_frames(&server, now_ms);
				break;
			}
		}
		else if (pfd.revents & POLLHUP)
		{
			/* If a delayed response is pending, the peer is gone and the
			 * response cannot be delivered. Drop it and stop instead of
			 * polling a hung-up fd until the delay expires (issue #19). */
			if (server.pending.active)
			{
				pending_clear(&server.pending);
				break;
			}
			if (server.have_rx && server.rx_len > 0)
				process_rx_frames(&server, now_ms);
			break;
		}

		if (!server.pending.active && server.have_rx && server.rx_len > 0 &&
		    now_ms - server.last_rx_ms >= gap_ms)
		{
			process_rx_frames(&server, now_ms);
			server.have_rx = server.rx_len > 0;
		}
	}

	LOG_INFO("RTU stopped");
	return 0;
}
