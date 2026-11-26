/* mogbus_probe(): one FC 08 Return Query Data round trip against a network server, used by
 * `mogbus --health`. Nonblocking sockets with poll() deadlines; no allocation. */
#include "mogbus.h"
#include "modbus/ascii.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "util/clock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROBE_TID	0x4D42

static const u8 PROBE_REQUEST[] = { FC_DIAGNOSTICS, 0x00, 0x00, 0xB0, 0x6B };

static void __attribute__((format(printf, 3, 4)))
probe_err(char* err, size_t err_size, const char* fmt, ...)
{
	va_list ap;

	if (!err || err_size == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, err_size, fmt, ap);
	va_end(ap);
}

static int
remaining_ms(u64 deadline)
{
	u64 now = clock_now_ms();

	return deadline > now ? (int)(deadline - now) : 0;
}

/* 1 = a complete reply from `unit` (*pdu points into buf or out), 0 = need more bytes,
 * -1 = malformed or from somebody else. */
static int
parse_reply(mogbus_mode_t mode, const u8* buf, u32 len, u8 unit, u8* out, const u8** pdu, u32* pdu_len)
{
	u8 addr = 0;

	switch (mode)
	{
		case MOGBUS_MODE_RTU_TCP:
		case MOGBUS_MODE_RTU_UDP:
			if (!modbus_rtu_decode(buf, len, &addr, pdu, pdu_len))
				return 0;
			return addr == unit ? 1 : -1;
		case MOGBUS_MODE_ASCII_TCP:
		case MOGBUS_MODE_ASCII_UDP:
		{
			u32 frame_len = 0;

			switch (modbus_ascii_scan(buf, len, &frame_len))
			{
				case MODBUS_ASCII_SCAN_NONE : return 0;
				case MODBUS_ASCII_SCAN_FRAME: break;
				default                     : return -1;
			}
			if (!modbus_ascii_decode(buf, frame_len, out, &addr, pdu, pdu_len))
				return -1;
			return addr == unit ? 1 : -1;
		}
		default:
		{
			mbap_header_t header;

			if (len < MBAP_HEADER_SIZE)
				return 0;
			mbap_decode(buf, &header);
			if (!mbap_is_valid(&header) ||
			    mbap_frame_size(&header) > MBAP_MAX_FRAME ||
			    header.transaction != PROBE_TID ||
			    header.unit != unit)
				return -1;
			if (len < mbap_frame_size(&header))
				return 0;
			*pdu     = buf + MBAP_HEADER_SIZE;
			*pdu_len = mbap_frame_size(&header) - MBAP_HEADER_SIZE;
			return 1;
		}
	}
}

bool
mogbus_probe(const mogbus_server_options_t* options, uint8_t unit_id, uint32_t timeout_ms,
	     char* err, size_t err_size)
{
	union
	{
		struct sockaddr		sa;
		struct sockaddr_in	v4;
		struct sockaddr_in6	v6;
	}		addr;
	socklen_t	addr_len;
	const char*	host;
	mogbus_mode_t	mode;
	bool		datagram;
	u8		frame[MODBUS_ASCII_MAX_FRAME];
	u8		rx[MODBUS_ASCII_MAX_FRAME];
	u8		decoded[MODBUS_MAX_PDU + 2];
	const u8*	pdu = NULL;
	u32		frame_len;
	u32		rx_len  = 0;
	u32		pdu_len = 0;
	u64		deadline;
	int		fd;
	bool		ok = false;

	if (!options)
		return false;
	mode     = options->mode;
	datagram = mode == MOGBUS_MODE_UDP ||
		   mode == MOGBUS_MODE_RTU_UDP ||
		   mode == MOGBUS_MODE_ASCII_UDP;
	if (!datagram &&
	    mode != MOGBUS_MODE_TCP &&
	    mode != MOGBUS_MODE_RTU_TCP &&
	    mode != MOGBUS_MODE_ASCII_TCP)
	{
		probe_err(err, err_size, "health check needs a network mode");
		return false;
	}
	if (unit_id == 0)
	{
		probe_err(err, err_size, "unit 0 is the broadcast address and never answers");
		return false;
	}

	/* A wildcard listener is reached through loopback. */
	host = options->bind_address ? options->bind_address : "127.0.0.1";
	if (strcmp(host, "0.0.0.0") == 0)
		host = "127.0.0.1";
	else if (strcmp(host, "::") == 0)
		host = "::1";
	memset(&addr, 0, sizeof(addr));
	if (inet_pton(AF_INET, host, &addr.v4.sin_addr) == 1)
	{
		addr.v4.sin_family = AF_INET;
		addr.v4.sin_port   = htons(options->port);
		addr_len           = sizeof(addr.v4);
	}
	else if (inet_pton(AF_INET6, host, &addr.v6.sin6_addr) == 1)
	{
		addr.v6.sin6_family = AF_INET6;
		addr.v6.sin6_port   = htons(options->port);
		addr_len            = sizeof(addr.v6);
	}
	else
	{
		probe_err(err, err_size, "bind address '%s' is not an IP address", host);
		return false;
	}

	switch (mode)
	{
		case MOGBUS_MODE_RTU_TCP:
		case MOGBUS_MODE_RTU_UDP:
			frame_len = modbus_rtu_encode(frame, unit_id, PROBE_REQUEST, sizeof(PROBE_REQUEST));
			break;
		case MOGBUS_MODE_ASCII_TCP:
		case MOGBUS_MODE_ASCII_UDP:
			frame_len = modbus_ascii_encode(frame, unit_id, PROBE_REQUEST, sizeof(PROBE_REQUEST));
			break;
		default:
		{
			mbap_header_t header = {
				.transaction = PROBE_TID,
				.length      = (u16)(sizeof(PROBE_REQUEST) + 1),
				.unit        = unit_id,
			};

			mbap_encode(frame, &header);
			memcpy(frame + MBAP_HEADER_SIZE, PROBE_REQUEST, sizeof(PROBE_REQUEST));
			frame_len = MBAP_HEADER_SIZE + sizeof(PROBE_REQUEST);
		}
	}

	deadline = clock_now_ms() + timeout_ms;
	fd = socket(addr.sa.sa_family, (datagram ? SOCK_DGRAM : SOCK_STREAM) | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		probe_err(err, err_size, "socket: %s", strerror(errno));
		return false;
	}
	if (connect(fd, &addr.sa, addr_len) < 0)
	{
		struct pollfd	pfd      = { .fd = fd, .events = POLLOUT };
		int		so_error = errno;
		socklen_t	so_len   = sizeof(so_error);

		if (so_error == EINPROGRESS)
		{
			if (poll(&pfd, 1, remaining_ms(deadline)) <= 0)
			{
				probe_err(err, err_size, "connect %s:%u: timed out", host, options->port);
				goto done;
			}
			if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0)
				so_error = errno;
		}
		if (so_error != 0)
		{
			probe_err(err, err_size, "connect %s:%u: %s", host, options->port, strerror(so_error));
			goto done;
		}
	}
	if (mode == MOGBUS_MODE_TCP && options->tls_enabled)
	{
		/* The handshake needs a client certificate the probe does not have: an
		 * accepted connection is as far as it goes. */
		ok = true;
		goto done;
	}
	if (send(fd, frame, frame_len, MSG_NOSIGNAL) != (ssize_t)frame_len)
	{
		probe_err(err, err_size, "send: %s", strerror(errno));
		goto done;
	}

	for (;;)
	{
		struct pollfd	pfd = { .fd = fd, .events = POLLIN };
		ssize_t		n;
		int		parsed;

		if (poll(&pfd, 1, remaining_ms(deadline)) <= 0)
		{
			probe_err(err, err_size, "no reply from unit %u within %u ms", unit_id, timeout_ms);
			goto done;
		}
		n = recv(fd, rx + rx_len, sizeof(rx) - rx_len, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n < 0)
		{
			probe_err(err, err_size, "recv: %s", strerror(errno));
			goto done;
		}
		if (n == 0 && !datagram)
		{
			probe_err(err, err_size, "connection closed without a reply");
			goto done;
		}
		rx_len += (u32)n;
		parsed  = parse_reply(mode, rx, rx_len, unit_id, decoded, &pdu, &pdu_len);
		if (parsed == 0 && !datagram && rx_len < sizeof(rx))
			continue;
		if (parsed != 1)
		{
			probe_err(err, err_size, "malformed reply from unit %u", unit_id);
			goto done;
		}
		break;
	}

	/* The echo is the healthy answer; an exception reply still proves the server and the
	 * unit are alive, so it counts too. */
	if (pdu_len >= 2 && pdu[0] == (FC_DIAGNOSTICS | 0x80))
		ok = true;
	else if (pdu_len == sizeof(PROBE_REQUEST) && memcmp(pdu, PROBE_REQUEST, pdu_len) == 0)
		ok = true;
	else
		probe_err(err, err_size, "unexpected reply to the diagnostic request");

done:
	close(fd);
	return ok;
}
