/**
 * @file
 * @brief Modbus client transport: framing, connection handling, deadlines and retries.
 *
 * Nonblocking descriptors with os_wait() deadlines. Stream carriers (TCP, serial) are closed on
 * a timeout so a late reply can never be taken for the next one; datagram carriers drop what
 * arrives before a request is sent.
 */
#include "client/client.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "modbus/rtu.h"
#include "os/os_socket.h"
#include "os/os_time.h"
#include "os/os_wait.h"
#include "vlib/arena.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** @brief Defaults and sizes of a client. */
enum
{
	CLIENT_DEFAULT_TIMEOUT_MS = 1000,
	CLIENT_HOST_MAX		  = 64,
	CLIENT_PATH_MAX		  = 128,
	CLIENT_ERROR_MAX	  = 256,
};

/** @brief Result of looking for the reply at the start of the receive buffer. */
enum
{
	PARSE_MORE  = 0,	/* incomplete */
	PARSE_REPLY = 1,	/* the reply to this request */
	PARSE_SKIP  = 2,	/* a frame that is not ours (stale, other unit): consume and go on */
	PARSE_BAD   = -1,	/* malformed */
};

/** @brief One client: its arena (which holds this struct), settings, descriptor and receive buffer. */
struct client_t
{
	arena_t*	arena;
	client_options_t o;
	char		host[CLIENT_HOST_MAX];
	char		path[CLIENT_PATH_MAX];
	os_addr_t	addr;
	os_socket_t	sock;
	os_serial_t	ser;
	os_serial_t	slave;		/* unused PTY slave of os_serial_open() */
	bool		connected;
	u16		tid;
	u8		exception;
	u32		rx_len;
	u8		rx[MODBUS_ASCII_MAX_FRAME];
	u8		decoded[MODBUS_MAX_PDU + 2];
	char		error[CLIENT_ERROR_MAX];
};

/* client_new() has no client to own its message, so each thread keeps the last one. */
static _Thread_local char detached_error[CLIENT_ERROR_MAX];

const char*
client_detached_error(void)
{
	return detached_error;
}

void
client_detached_errorf(const char* fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(detached_error, sizeof(detached_error), fmt, ap);
	va_end(ap);
}

const char*
client_error(const client_t* c)
{
	return c ? c->error : "";
}

u8
client_exception(const client_t* c)
{
	return c ? c->exception : 0;
}

static client_status_t __attribute__((format(printf, 3, 4)))
fail(client_t* c, client_status_t status, const char* fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(c->error, sizeof(c->error), fmt, ap);
	va_end(ap);
	return status;
}

void
client_set_error(client_t* c, const char* fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(c->error, sizeof(c->error), fmt, ap);
	va_end(ap);
}

static bool
is_serial(const client_t* c)
{
	return c->o.mode == CLIENT_RTU_SERIAL || c->o.mode == CLIENT_ASCII_SERIAL;
}

static bool
is_datagram(const client_t* c)
{
	return c->o.mode == CLIENT_UDP ||
	       c->o.mode == CLIENT_RTU_UDP ||
	       c->o.mode == CLIENT_ASCII_UDP;
}

static bool
is_rtu(const client_t* c)
{
	return c->o.mode == CLIENT_RTU_TCP ||
	       c->o.mode == CLIENT_RTU_UDP ||
	       c->o.mode == CLIENT_RTU_SERIAL;
}

static bool
is_ascii(const client_t* c)
{
	return c->o.mode == CLIENT_ASCII_TCP ||
	       c->o.mode == CLIENT_ASCII_UDP ||
	       c->o.mode == CLIENT_ASCII_SERIAL;
}

client_t*
client_new(const client_options_t* options)
{
	arena_t*	arena;
	client_t*	c;
	const char*	host;

	detached_error[0] = '\0';
	if (!options)
	{
		client_detached_errorf("options are required");
		return NULL;
	}
	if (options->mode > CLIENT_ASCII_SERIAL)
	{
		client_detached_errorf("unknown client mode %d", (int)options->mode);
		return NULL;
	}
	if (options->tls)
	{
		client_detached_errorf("TLS is not supported by the client");
		return NULL;
	}
	arena = ARENA_ALLOC();
	c     = arena ? ARENA_PUSH_ARRAY_ALIGNED(arena, client_t, 1, 16) : NULL;
	if (!c)
	{
		if (arena)
			arena_release(arena);
		client_detached_errorf("out of memory");
		return NULL;
	}
	c->arena = arena;
	c->o     = *options;
	c->sock  = OS_SOCKET_INVALID;
	c->ser   = OS_SERIAL_INVALID;
	c->slave = OS_SERIAL_INVALID;
	if (c->o.timeout_ms == 0)
		c->o.timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

	if (is_serial(c))
	{
		if (!options->path ||
		    options->path[0] == '\0' ||
		    strlen(options->path) >= sizeof(c->path))
		{
			client_detached_errorf("serial modes need a device path of 1 to %zu characters",
					       sizeof(c->path) - 1);
			goto bad;
		}
		if (!os_serial_supported())
		{
			client_detached_errorf("serial lines are not supported on this platform");
			goto bad;
		}
		if ((options->data_bits != 7 && options->data_bits != 8) ||
		    (is_rtu(c) && options->data_bits != 8) ||
		    (options->stop_bits != 1 && options->stop_bits != 2) ||
		    options->parity > PARITY_SPACE ||
		    (options->baud != 0 && !os_baud_is_standard(options->baud)))
		{
			client_detached_errorf("invalid serial line settings (8 data bits for RTU, 7 or 8 "
					       "for ASCII, 1 or 2 stop bits, a standard baud rate)");
			goto bad;
		}
		snprintf(c->path, sizeof(c->path), "%s", options->path);
		return c;
	}

	host = options->host ? options->host : "127.0.0.1";
	if (strcmp(host, "localhost") == 0)
		host = "127.0.0.1";
	if (options->port == 0)
	{
		client_detached_errorf("a port is required");
		goto bad;
	}
	if (strlen(host) >= sizeof(c->host) ||
	    !os_addr_parse(&c->addr, host, options->port))
	{
		client_detached_errorf("host '%s' is not an IP address", host);
		goto bad;
	}
	snprintf(c->host, sizeof(c->host), "%s", host);
	return c;

bad:
	arena_release(arena);
	return NULL;
}

static void
disconnect(client_t* c)
{
	if (os_socket_valid(c->sock))
		os_socket_close(c->sock);
	if (c->ser != OS_SERIAL_INVALID)
		os_serial_close(c->ser);
	if (c->slave != OS_SERIAL_INVALID)
		os_serial_close(c->slave);
	c->sock      = OS_SOCKET_INVALID;
	c->ser       = OS_SERIAL_INVALID;
	c->slave     = OS_SERIAL_INVALID;
	c->connected = false;
	c->rx_len    = 0;
}

void
client_free(client_t* c)
{
	if (!c)
		return;
	disconnect(c);
	arena_release(c->arena);	/* the client lives in its own arena */
}

static int
remaining_ms(u64 deadline)
{
	u64 now = os_now_ms();

	return deadline > now ? (int)(deadline - now) : 0;
}

/* Waits for events on the carrier: > 0 ready, 0 timed out, -1 failed. */
static int
wait_for(const client_t* c, u32 events, u64 deadline)
{
	os_wait_entry_t pfd = { .fd = os_socket_valid(c->sock) ? c->sock : (os_socket_t)c->ser,
				.kind = is_serial(c) ? OS_WAIT_SERIAL : OS_WAIT_SOCKET,
				.events = events };

	for (;;)
	{
		int n = os_wait(&pfd, 1, remaining_ms(deadline));

		if (n < 0 && os_interrupted())
			continue;
		return n;
	}
}

static s64
carrier_read(const client_t* c, void* buf, u32 len)
{
	return is_serial(c) ? os_serial_read(c->ser, buf, len) : os_socket_recv(c->sock, buf, len);
}

static s64
carrier_write(const client_t* c, const void* buf, u32 len)
{
	return is_serial(c) ? os_serial_write(c->ser, buf, len) : os_socket_send(c->sock, buf, len);
}

static client_status_t
connect_until(client_t* c, u64 deadline)
{
	if (c->connected)
		return CLIENT_OK;
	c->rx_len = 0;
	if (is_serial(c))
	{
		c->ser = os_serial_open(false, "client", c->path, c->o.baud, c->o.data_bits,
					c->o.parity, c->o.stop_bits, &c->slave, NULL, 0);
		if (c->ser == OS_SERIAL_INVALID)
			return fail(c, CLIENT_IO, "cannot open serial device '%s'", c->path);
	}
	else
	{
		int rc;
		int so_error;

		c->sock = os_socket_open(&c->addr, is_datagram(c));
		if (!os_socket_valid(c->sock))
			return fail(c, CLIENT_IO, "socket: %s", os_last_error_text());
		rc       = os_connect(c->sock, &c->addr);
		so_error = rc < 0 ? os_last_error() : 0;
		if (rc > 0)
		{
			if (wait_for(c, OS_WAIT_OUT, deadline) <= 0)
			{
				disconnect(c);
				return fail(c, CLIENT_IO, "connect %s:%u: timed out", c->host, c->o.port);
			}
			so_error = os_connect_status(c->sock);
		}
		if (so_error != 0)
		{
			disconnect(c);
			return fail(c, CLIENT_IO, "connect %s:%u: %s", c->host, c->o.port,
				    os_error_text(so_error));
		}
		if (!is_datagram(c))
			(void)os_tcp_nodelay(c->sock);
	}
	c->connected = true;
	return CLIENT_OK;
}

client_status_t
client_connect(client_t* c)
{
	if (!c)
		return CLIENT_INVALID;
	return connect_until(c, os_now_ms() + c->o.timeout_ms);
}

/* Reply length of an RTU response whose first bytes are in buf: 1 known (*need), 0 need more
 * bytes to tell, -1 not derivable (user-defined or encapsulated function code). */
static int
rtu_reply_len(const u8* buf, u32 len, u32* need)
{
	u8 fc;

	if (len < 2)
		return 0;
	fc = buf[1];
	if (fc & 0x80)
	{
		*need = 5;
		return 1;
	}
	switch (fc)
	{
		case 0x05: case 0x06: case 0x08: case 0x0B: case 0x0F: case 0x10:
			*need = 8;
			return 1;
		case 0x16:
			*need = 10;
			return 1;
		case 0x07:
			*need = 5;
			return 1;
		case 0x01: case 0x02: case 0x03: case 0x04: case 0x0C: case 0x11:
		case 0x14: case 0x15: case 0x17:
			if (len < 3)
				return 0;
			*need = 5u + buf[2];
			return 1;
		case 0x18:
			if (len < 4)
				return 0;
			*need = 6u + get_be16(buf + 2);
			return 1;
		default:
			return -1;
	}
}

/* Looks for the reply to (unit, tid) at the start of c->rx. */
static int
parse(client_t* c, u8 unit, u16 tid, const u8** pdu, u32* pdu_len, u32* used)
{
	const u8*	rx  = c->rx;
	u32		len = c->rx_len;
	u8		addr = 0;

	if (is_rtu(c))
	{
		u32 need = len;
		int known = is_datagram(c) ? 0 : rtu_reply_len(rx, len, &need);

		if (known == 0 && !is_datagram(c))
			return PARSE_MORE;
		if (known == 1 && len < need)
			return PARSE_MORE;
		if (!modbus_rtu_decode(rx, need, &addr, pdu, pdu_len))
			return known == 1 ? PARSE_BAD : PARSE_MORE;
		*used = need;
		return addr == unit ? PARSE_REPLY : PARSE_SKIP;
	}
	if (is_ascii(c))
	{
		u32 frame_len = 0;

		switch (modbus_ascii_scan(rx, len, &frame_len))
		{
			case MODBUS_ASCII_SCAN_NONE:
				return PARSE_MORE;
			case MODBUS_ASCII_SCAN_FRAME:
				break;
			default:
				*used = frame_len;
				return PARSE_SKIP;
		}
		*used = frame_len;
		if (!modbus_ascii_decode(rx, frame_len, c->decoded, &addr, pdu, pdu_len))
			return PARSE_BAD;
		return addr == unit ? PARSE_REPLY : PARSE_SKIP;
	}
	{
		mbap_header_t header;

		if (len < MBAP_HEADER_SIZE)
			return PARSE_MORE;
		mbap_decode(rx, &header);
		if (!mbap_is_valid(&header) ||
		    mbap_frame_size(&header) > MBAP_MAX_FRAME)
			return PARSE_BAD;
		if (len < mbap_frame_size(&header))
			return PARSE_MORE;
		*used    = mbap_frame_size(&header);
		*pdu     = rx + MBAP_HEADER_SIZE;
		*pdu_len = *used - MBAP_HEADER_SIZE;
		if (header.transaction != tid)
			return PARSE_SKIP;
		return header.unit == unit ? PARSE_REPLY : PARSE_BAD;
	}
}

/* One attempt: connect if needed, send, wait for the reply PDU (copied to reply). */
static client_status_t
exchange(client_t* c, u8 unit, const u8* req, u32 req_len, bool broadcast,
	 u8* reply, u32 reply_cap, u32* reply_len)
{
	u64		deadline = os_now_ms() + c->o.timeout_ms;
	u8		frame[MODBUS_ASCII_MAX_FRAME];
	u32		frame_len;
	u32		sent = 0;
	client_status_t	st;
	u8		scratch[16];

	st = connect_until(c, deadline);
	if (st != CLIENT_OK)
		return st;

	if (is_rtu(c))
		frame_len = modbus_rtu_encode(frame, unit, req, req_len);
	else if (is_ascii(c))
		frame_len = modbus_ascii_encode(frame, unit, req, req_len);
	else
	{
		mbap_header_t header = { .transaction = ++c->tid,
					 .length      = (u16)(req_len + 1),
					 .unit        = unit };

		mbap_encode(frame, &header);
		memcpy(frame + MBAP_HEADER_SIZE, req, req_len);
		frame_len = MBAP_HEADER_SIZE + req_len;
	}

	/* Anything already waiting belongs to an earlier request. */
	c->rx_len = 0;
	if (is_datagram(c) || is_serial(c))
		while (carrier_read(c, scratch, sizeof(scratch)) > 0)
			;

	while (sent < frame_len)
	{
		s64 n = carrier_write(c, frame + sent, frame_len - sent);

		if (n < 0 && os_retry())
		{
			if (wait_for(c, OS_WAIT_OUT, deadline) <= 0)
			{
				disconnect(c);
				return fail(c, CLIENT_IO, "send: timed out");
			}
			continue;
		}
		if (n <= 0)
		{
			fail(c, CLIENT_IO, "send: %s", os_last_error_text());
			disconnect(c);
			return CLIENT_IO;
		}
		sent += (u32)n;
	}
	if (broadcast)
	{
		*reply_len = 0;
		return CLIENT_OK;
	}

	for (;;)
	{
		const u8*	pdu     = NULL;
		u32		pdu_len = 0;
		u32		used    = 0;
		int		r       = c->rx_len ? parse(c, unit, c->tid, &pdu, &pdu_len, &used) : PARSE_MORE;
		s64		n;

		if (r == PARSE_REPLY)
		{
			if (pdu_len > reply_cap)
				return fail(c, CLIENT_FRAME, "reply of %u bytes does not fit the %u-byte buffer",
					    pdu_len, reply_cap);
			memcpy(reply, pdu, pdu_len);
			*reply_len = pdu_len;
			c->rx_len  = 0;
			return CLIENT_OK;
		}
		if (r == PARSE_SKIP)
		{
			memmove(c->rx, c->rx + used, c->rx_len - used);
			c->rx_len -= used;
			continue;
		}
		if (r == PARSE_BAD && !is_datagram(c))
		{
			fail(c, CLIENT_FRAME, "malformed reply from unit %u", unit);
			disconnect(c);
			return CLIENT_FRAME;
		}
		if (r == PARSE_BAD || (r == PARSE_MORE && is_datagram(c)))
			c->rx_len = 0;	/* a datagram is one frame: a bad or short one is dropped */
		if (c->rx_len == sizeof(c->rx))
		{
			fail(c, CLIENT_FRAME, "reply from unit %u is longer than any frame", unit);
			disconnect(c);
			return CLIENT_FRAME;
		}

		if (wait_for(c, OS_WAIT_IN, deadline) <= 0)
		{
			if (!is_datagram(c) && !is_serial(c))
				disconnect(c);	/* a late reply must not answer the next request */
			return fail(c, CLIENT_TIMEOUT, "no reply from unit %u within %u ms", unit,
				    c->o.timeout_ms);
		}
		n = carrier_read(c, c->rx + c->rx_len, (u32)sizeof(c->rx) - c->rx_len);
		if (n < 0 && os_retry())
			continue;
		if (n < 0 || (n == 0 && !is_datagram(c)))
		{
			if (n < 0)
				fail(c, CLIENT_IO, "recv: %s", os_last_error_text());
			else
				fail(c, CLIENT_IO, "connection closed without a reply");
			disconnect(c);
			return CLIENT_IO;
		}
		c->rx_len += (u32)n;
	}
}

client_status_t
client_request(client_t* c, u8 unit, const u8* req, u32 req_len, u8* reply, u32 reply_cap, u32* reply_len)
{
	client_status_t	st;
	bool		broadcast;
	u32		attempts;

	if (!c)
		return CLIENT_INVALID;
	c->error[0] = '\0';
	if (!req || req_len == 0 || req_len > MODBUS_MAX_PDU || !reply_len || (reply_cap && !reply))
		return fail(c, CLIENT_INVALID, "a request needs 1 to %d bytes and a reply length", MODBUS_MAX_PDU);
	broadcast = unit == 0 && (is_rtu(c) || is_ascii(c));
	if (unit == 0 && !broadcast)
		return fail(c, CLIENT_INVALID, "unit 0 is only the broadcast address in the RTU and ASCII modes");
	if ((is_rtu(c) || is_ascii(c)) && unit > 247)
		return fail(c, CLIENT_INVALID, "unit %u is outside 1-247", unit);

	*reply_len = 0;
	attempts   = 1 + c->o.retries;
	do
		st = exchange(c, unit, req, req_len, broadcast, reply, reply_cap, reply_len);
	while (st == CLIENT_TIMEOUT && --attempts > 0);
	if (st != CLIENT_OK)
		return st;
	if (broadcast)
		return CLIENT_OK;

	if (*reply_len >= 1 && reply[0] == (req[0] | 0x80))
	{
		if (*reply_len < 2)
			return fail(c, CLIENT_FRAME, "exception reply without a code");
		c->exception = reply[1];
		return fail(c, CLIENT_EXCEPTION, "unit %u answered function code 0x%02X with exception 0x%02X",
			    unit, req[0], reply[1]);
	}
	if (*reply_len < 1 || reply[0] != req[0])
		return fail(c, CLIENT_FRAME, "reply to function code 0x%02X has function code 0x%02X",
			    req[0], *reply_len ? reply[0] : 0);
	return CLIENT_OK;
}
