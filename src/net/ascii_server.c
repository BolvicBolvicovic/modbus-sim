#define _GNU_SOURCE
#include "net/ascii_server.h"
#include "modbus/ascii.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/**
 * @file
 * @brief Modbus ASCII server on a serial-like byte stream.
 *
 * ASCII frames are delimited by CR LF, so unlike RTU there is no inter-frame silence timing.
 * Complete frames are dispatched as soon as the terminator arrives, including several frames
 * delivered in one read(). A partial frame with no further bytes for one second is dropped so
 * it cannot corrupt a later request.
 */

enum
{
	ASCII_INTERCHAR_TIMEOUT_MS = 1000,
};

static void
ascii_consume(ascii_server_t* server, u32 n)
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
ascii_handle_frame(ascii_server_t* server, u64 now_ms, const u8* frame, u32 len)
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
		net_record_comm_error(server->options.obs, server->options.devices, server->options.device_count);
		return;
	}
	dev = addr ? net_find_device(server->options.devices, server->options.device_count, addr) : NULL;
	if (!dev)
	{
		/* broadcast or unknown unit: no response; never reaches a device */
		net_obs_request(server->options.obs, NULL, NULL, addr, pdu[0], NULL, 0, NULL);
		return;
	}

	resp_len = net_handle_pdu(server->options.obs, NULL, dev, &server->rng, now_ms, pdu, pdu_len,
				  resp, &decision);
	if (decision.drop)
		return;

	frame_len = modbus_ascii_encode(server->pending.buf, addr, resp, resp_len);
	if (frame_len == 0)
		return;
	/* A checksum fault only damages the emitted LRC; the PDU stays intact. */
	if (decision.corrupt == FAULT_CORRUPT_CHECKSUM)
		modbus_ascii_corrupt_lrc(server->pending.buf, frame_len);
	server->pending.active = true;
	server->pending.len = frame_len;
	server->pending.sent = 0;
	server->pending.due_ms = now_ms + decision.delay_ms;
}

static void
ascii_process(ascii_server_t* server, u64 now_ms)
{
	const u8*	rx = NULL;
	u32		remaining = 0;
	bool		have_frame = false;

	/* Inter-frame whitespace is not a malformed frame. Drop it before the
	 * look-ahead scan and the partial-frame timeout below. */
	while (server->rx_len > 0)
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(server->rx, server->rx_len, &len);

		if (scan != MODBUS_ASCII_SCAN_SKIP || len == 0)
			break;
		ascii_consume(server, len);
	}

	rx = server->rx;
	remaining = server->rx_len;

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
	    server->rx_len > 0 &&
	    server->last_rx_ms > 0)
	{
		u64 idle_ms = now_ms >= server->last_rx_ms ?
			      now_ms - server->last_rx_ms : 0;

		if (idle_ms >= ASCII_INTERCHAR_TIMEOUT_MS)
		{
			/* A stale partial frame would otherwise merge with the next
			 * request; count it like any other discarded frame. */
			server->rx_len = 0;
			server->last_rx_ms = 0;
			net_record_comm_error(server->options.obs, server->options.devices, server->options.device_count);
		}
	}

	while (server->rx_len > 0 && !server->pending.active)
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(server->rx, server->rx_len, &len);

		if (scan == MODBUS_ASCII_SCAN_SKIP)
		{
			ascii_consume(server, len);
			continue;
		}
		if (scan == MODBUS_ASCII_SCAN_NONE || len == 0)
			break;
		if (scan == MODBUS_ASCII_SCAN_FRAME)
			ascii_handle_frame(server, now_ms, server->rx, len);
		else
			net_record_comm_error(server->options.obs, server->options.devices, server->options.device_count);
		ascii_consume(server, len);
	}
}

static bool
ascii_read_available(ascii_server_t* server, u64 now_ms)
{
	ssize_t got;

	if (server->read_closed)
		return false;
	if (server->rx_len == sizeof(server->rx))
		ascii_process(server, now_ms);
	if (server->rx_len == sizeof(server->rx))
		server->rx_len = 0; /* cannot resync a full buffer without a colon */

	got = read(server->options.fd, server->rx + server->rx_len,
		   sizeof(server->rx) - server->rx_len);
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
	server->rx_len += (u32)got;
	server->last_rx_ms = now_ms;
	return true;
}

static bool
ascii_write_pending(ascii_server_t* server)
{
	pending_response_t*	p = &server->pending;
	ssize_t		sent;

	if (!p->active || p->due_ms > clock_now_ms())
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
ascii_build_poll(const ascii_server_t* server, struct pollfd* pfd, int* timeout, u64 now_ms)
{
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
}

static bool
ascii_handle_revents(ascii_server_t* server, short revents, u64 now_ms)
{
	if (revents & (POLLERR | POLLNVAL))
		return false;
	ascii_process(server, now_ms); /* expire stale bytes before adding new ones */
	if ((revents & POLLOUT) && !ascii_write_pending(server))
		return false;
	if (revents & POLLIN)
		(void)ascii_read_available(server, now_ms);
	ascii_process(server, now_ms);
	if (revents & POLLHUP)
		return net_handle_hangup(&server->pending);
	if (server->read_closed && !server->pending.active)
		return false;
	return true;
}

static void
ascii_log_startup(const ascii_server_options_t* options)
{
	LOG_INFO("ASCII listening on fd %d (%u device%s). Ctrl-C to stop.",
		 options->fd, options->device_count, options->device_count == 1 ? "" : "s");
}

s32
ascii_server_step(ascii_server_t* server, s32 max_wait_ms)
{
	struct pollfd	pfd = { 0 };
	int		timeout = 0;
	u64		now_ms = clock_now_ms();

	if (server->stop)
		return NET_STEP_DONE;

	ascii_build_poll(server, &pfd, &timeout, now_ms);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (net_poll(server->options.obs, &pfd, 1, timeout) < 0)
	{
		if (errno == EINTR)
			return NET_STEP_CONTINUE;	/* the caller re-checks stop */
		LOG_WARN("ASCII poll failed: %s", strerror(errno));
		return NET_STEP_ERROR;
	}
	now_ms = clock_now_ms();
	return ascii_handle_revents(server, pfd.revents, now_ms) ?
	       NET_STEP_CONTINUE : NET_STEP_DONE;
}

void
ascii_server_stop(ascii_server_t* server)
{
	server->stop = 1;
}

s32
ascii_server_init(ascii_server_t* server, const ascii_server_options_t* options)
{
	memset(server, 0, sizeof(*server));
	server->options = *options;
	rng_seed(&server->rng, options->seed);
	ascii_log_startup(options);
	server->open = true;
	return 0;
}

void
ascii_server_close(ascii_server_t* server)
{
	if (!server->open)
		return;
	server->open = false;
	LOG_INFO("ASCII stopped");
}

s32
ascii_server_run(ascii_server_t* server, const ascii_server_options_t* options)
{
	s32 rc = ascii_server_init(server, options);

	if (rc != 0)
		return rc;
	while ((rc = ascii_server_step(server, -1)) == NET_STEP_CONTINUE)
		;
	ascii_server_close(server);
	return rc == NET_STEP_ERROR ? 1 : 0;
}
