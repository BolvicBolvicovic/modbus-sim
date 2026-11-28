/**
 * @file
 * @brief mogbus_probe(): one FC 08 Return Query Data round trip against a network server, used
 *        by `mogbus --health`.
 *
 * Nonblocking sockets with os_wait() deadlines; no allocation, except that a TLS server that
 * does not require client certificates is probed through a client (its own arena and OpenSSL).
 */
#include "mogbus.h"
#include "client/client.h"
#include "modbus/ascii.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "os/os_socket.h"
#include "os/os_time.h"
#include "os/os_wait.h"
#include <string.h>

#define PROBE_TID	0x4D42

static const u8 PROBE_REQUEST[] = { FC_DIAGNOSTICS, 0x00, 0x00, 0xB0, 0x6B };

#define probe_err client_detached_errorf

static int
remaining_ms(u64 deadline)
{
	u64 now = os_now_ms();

	return deadline > now ? (int)(deadline - now) : 0;
}

/**
 * @brief 1 = a complete reply from `unit` (*pdu points into buf or out), 0 = need more bytes,
 *        -1 = malformed or from somebody else.
 */
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

mogbus_result_t
mogbus_probe(const mogbus_server_options_t* options, uint8_t unit_id, uint32_t timeout_ms)
{
	os_addr_t	addr;
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
	os_socket_t	fd;
	bool		ok = false;

	client_detached_errorf("");
	if (!options)
	{
		probe_err("options are required");
		return MOGBUS_ERR_INVALID;
	}
	mode     = options->mode;
	datagram = mode == MOGBUS_MODE_UDP ||
		   mode == MOGBUS_MODE_RTU_UDP ||
		   mode == MOGBUS_MODE_ASCII_UDP;
	if (!datagram &&
	    mode != MOGBUS_MODE_TCP &&
	    mode != MOGBUS_MODE_RTU_TCP &&
	    mode != MOGBUS_MODE_ASCII_TCP)
	{
		probe_err("health check needs a network mode");
		return MOGBUS_ERR_INVALID;
	}
	if (unit_id == 0)
	{
		probe_err("unit 0 is the broadcast address and never answers");
		return MOGBUS_ERR_INVALID;
	}

	/* A wildcard listener is reached through loopback. */
	host = options->bind_address ? options->bind_address : "127.0.0.1";
	if (strcmp(host, "0.0.0.0") == 0)
		host = "127.0.0.1";
	else if (strcmp(host, "::") == 0)
		host = "::1";
	if (!os_addr_parse(&addr, host, options->port))
	{
		probe_err("bind address '%s' is not an IP address", host);
		return MOGBUS_ERR_INVALID;
	}

	if (mode == MOGBUS_MODE_TCP &&
	    options->tls_enabled &&
	    !options->tls_require_client_certificate)
	{
		/* A real echo over TLS, without a client certificate and without verifying the
		 * server: the probe only asks whether the local listener is alive. */
		client_options_t	co = { .mode            = CLIENT_TCP,
					       .host            = host,
					       .port            = options->port,
					       .tls             = true,
					       .timeout_ms      = timeout_ms };
		client_t*		client = NULL;
		u8			reply[MODBUS_MAX_PDU];
		u32			reply_len = 0;
		client_status_t		st;

		if (client_new(&co, &client) != CLIENT_OK)
			return MOGBUS_ERR_PROBE;	/* the text is client_detached_error() */
		st = client_request(client, unit_id, PROBE_REQUEST, sizeof(PROBE_REQUEST), reply,
				    sizeof(reply), &reply_len);
		if (st != CLIENT_OK && st != CLIENT_EXCEPTION)
			probe_err("%s", client_error(client));
		client_free(client);
		return st == CLIENT_OK || st == CLIENT_EXCEPTION ? MOGBUS_OK : MOGBUS_ERR_PROBE;
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

	deadline = os_now_ms() + timeout_ms;
	fd = os_socket_open(&addr, datagram);
	if (fd == OS_SOCKET_INVALID)
	{
		probe_err("socket: %s", os_last_error_text());
		return MOGBUS_ERR_PROBE;
	}
	{
		int	rc       = os_connect(fd, &addr);
		int	so_error = rc < 0 ? os_last_error() : 0;

		if (rc > 0)
		{
			os_wait_entry_t pfd = { .fd = fd, .events = OS_WAIT_OUT };

			if (os_wait(&pfd, 1, remaining_ms(deadline)) <= 0)
			{
				probe_err("connect %s:%u: timed out", host, options->port);
				goto done;
			}
			so_error = os_connect_status(fd);
		}
		if (so_error != 0)
		{
			probe_err("connect %s:%u: %s", host, options->port, os_error_text(so_error));
			goto done;
		}
	}
	if (mode == MOGBUS_MODE_TCP && options->tls_enabled)
	{
		/* Client certificates are required and the probe has none (the server's own
		 * certificate is usually not valid for client authentication): an accepted
		 * connection is as far as it goes. */
		ok = true;
		goto done;
	}
	if (os_socket_send(fd, frame, frame_len) != (s64)frame_len)
	{
		probe_err("send: %s", os_last_error_text());
		goto done;
	}

	for (;;)
	{
		os_wait_entry_t	pfd = { .fd = fd, .events = OS_WAIT_IN };
		s64		n;
		int		parsed;

		if (os_wait(&pfd, 1, remaining_ms(deadline)) <= 0)
		{
			probe_err("no reply from unit %u within %u ms", unit_id, timeout_ms);
			goto done;
		}
		n = os_socket_recv(fd, rx + rx_len, sizeof(rx) - rx_len);
		if (n < 0 && os_retry())
			continue;
		if (n < 0)
		{
			probe_err("recv: %s", os_last_error_text());
			goto done;
		}
		if (n == 0 && !datagram)
		{
			probe_err("connection closed without a reply");
			goto done;
		}
		rx_len += (u32)n;
		parsed  = parse_reply(mode, rx, rx_len, unit_id, decoded, &pdu, &pdu_len);
		if (parsed == 0 && !datagram && rx_len < sizeof(rx))
			continue;
		if (parsed != 1)
		{
			probe_err("malformed reply from unit %u", unit_id);
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
		probe_err("unexpected reply to the diagnostic request");

done:
	os_socket_close(fd);
	return ok ? MOGBUS_OK : MOGBUS_ERR_PROBE;
}
