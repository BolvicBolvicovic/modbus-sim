#define _GNU_SOURCE
#include "net/ascii_server.h"
#include "modbus/ascii.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* ASCII frames are delimited by CRLF, so unlike RTU there is no inter-frame
 * silence timing. Complete frames are dispatched as soon as the terminator
 * arrives, including several frames delivered in one read(). */

typedef struct ascii_server_t ascii_server_t;
struct ascii_server_t
{
	ascii_server_options_t	options;
	rng_t			rng;
	u8			rx[MODBUS_ASCII_MAX_FRAME];
	u32			rx_len;
	pending_response_t	pending;
	bool			read_closed;
};

static volatile sig_atomic_t	g_ascii_stop = 0;

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
		return;
	if (addr == 0)
		return; /* broadcast: no response */
	dev = net_find_device(server->options.devices, server->options.device_count, addr);
	if (!dev)
		return; /* unknown unit: no response */

	resp_len = net_handle_pdu(dev, &server->rng, now_ms, pdu, pdu_len, resp, &decision);
	LOG_DEBUG("ascii unit=%u fc=0x%02X -> %s%s", addr, pdu[0],
		  (resp[0] & 0x80) ? "exception" : "ok",
		  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");
	if (decision.drop)
		return;

	frame_len = modbus_ascii_encode(server->pending.buf, addr, resp, resp_len);
	if (frame_len == 0)
		return;
	server->pending.active = true;
	server->pending.len = frame_len;
	server->pending.sent = 0;
	server->pending.due_ms = now_ms + decision.delay_ms;
}

static void
ascii_process(ascii_server_t* server, u64 now_ms)
{
	while (server->rx_len > 0 && !server->pending.active)
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(server->rx, server->rx_len, &len);

		if (scan == MODBUS_ASCII_SCAN_NONE || len == 0)
			break;
		if (scan == MODBUS_ASCII_SCAN_FRAME)
			ascii_handle_frame(server, now_ms, server->rx, len);
		ascii_consume(server, len);
	}
}

static bool
ascii_read_available(ascii_server_t* server)
{
	ssize_t got;

	if (server->read_closed)
		return false;
	if (server->rx_len == sizeof(server->rx))
		ascii_process(server, clock_now_ms());
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
	if ((revents & POLLOUT) && !ascii_write_pending(server))
		return false;
	if (revents & POLLIN)
		(void)ascii_read_available(server);
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
ascii_server_run(const ascii_server_options_t* options)
{
	ascii_server_t server;

	memset(&server, 0, sizeof(server));
	server.options = *options;
	rng_seed(&server.rng, options->seed);
	g_ascii_stop = 0;
	net_install_stop_handlers(&g_ascii_stop);
	ascii_log_startup(options);

	while (!g_ascii_stop)
	{
		struct pollfd	pfd = { 0 };
		int		timeout = 0;
		u64		now_ms = clock_now_ms();

		ascii_build_poll(&server, &pfd, &timeout, now_ms);
		if (poll(&pfd, 1, timeout) < 0)
		{
			if (errno == EINTR)
				continue;
			LOG_WARN("ASCII poll failed: %s", strerror(errno));
			return 1;
		}
		now_ms = clock_now_ms();
		if (!ascii_handle_revents(&server, pfd.revents, now_ms))
			break;
	}

	LOG_INFO("ASCII stopped");
	return 0;
}
