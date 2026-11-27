/**
 * @file
 * @brief Modbus ASCII over UDP: one frame per datagram.
 */
#define _GNU_SOURCE
#include "net/ascii_udp_server.h"
#include "modbus/pdu.h"
#include "modbus/ascii.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "net/net_udp.h"
#include "os/os_time.h"
#include "os/os_wait.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <string.h>

/**
 * @brief One datagram is exactly one complete frame ':'...CR LF: no reassembly, so a missing
 *        terminator, trailing bytes or a second frame make it malformed.
 */
static void
handle_datagram(ascii_udp_server_t* server, const net_udp_peer_t* peer, u8* buf, u32 len)
{
	u8			decoded[MODBUS_MAX_PDU + 2];
	u8			addr     = 0;
	const u8*		pdu      = NULL;
	u32			pdu_len  = 0;
	u32			scan_len = 0;
	u8			resp[MODBUS_MAX_PDU];
	u8			out[MODBUS_ASCII_MAX_FRAME];
	u32			resp_len;
	u32			out_len;
	device_t*		dev;
	fault_decision_t	decision = { 0 };
	u64			now_ms   = os_now_ms();

	len = net_frame_trace(server->options.obs, NET_FRAME_RECEIVED, peer->text, buf, len,
			      MODBUS_ASCII_MAX_FRAME);
	if (len == 0)
		return;
	if (modbus_ascii_scan(buf, len, &scan_len) != MODBUS_ASCII_SCAN_FRAME ||
	    scan_len != len ||
	    !modbus_ascii_decode(buf, len, decoded, &addr, &pdu, &pdu_len))
	{
		LOG_DEBUG("ascii-udp %s sent a malformed datagram (%u bytes), ignored", peer->text, len);
		net_record_comm_error(server->options.obs, server->options.devices, server->options.device_count);
		return;
	}
	dev = addr ? net_find_device(server->options.devices, server->options.device_count, addr, true) : NULL;
	if (!dev)
	{
		/* broadcast or unknown unit: no response, no request processing; never reaches a device */
		net_obs_request(server->options.obs, peer->text, NULL, addr, pdu[0], NULL, 0, NULL);
		return;
	}

	resp_len = net_handle_pdu(server->options.obs, peer->text, dev, addr, &server->rng, now_ms, pdu, pdu_len,
				  resp, &decision);
	if (decision.drop)
		return;

	out_len = modbus_ascii_encode(out, addr, resp, resp_len);
	if (out_len == 0)
		return;
	/* A checksum fault only damages the emitted LRC; the PDU stays intact. */
	if (decision.corrupt == FAULT_CORRUPT_CHECKSUM)
		modbus_ascii_corrupt_lrc(out, out_len);
	out_len = net_frame_trace(server->options.obs, NET_FRAME_SENT, peer->text, out, out_len,
				  MODBUS_ASCII_MAX_FRAME);
	if (out_len == 0)
		return;
	(void)net_udp_send_at(&server->udp, peer, out, out_len, now_ms + decision.delay_ms, now_ms);
}

s32
ascii_udp_server_step(ascii_udp_server_t* server, s32 max_wait_ms)
{
	os_wait_entry_t	pfd     = { .fd = server->udp.fd, .events = OS_WAIT_IN };
	int		timeout = 1000;
	u64		now_ms  = os_now_ms();

	if (server->stop)
		return NET_STEP_DONE;

	net_udp_flush_due(&server->udp, now_ms);
	net_udp_poll_timeout(&server->udp, now_ms, &timeout);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (net_poll(server->options.obs, &pfd, 1, timeout) < 0)
		return NET_STEP_CONTINUE;	/* EINTR: the caller re-checks stop */

	net_udp_flush_due(&server->udp, os_now_ms());
	if (!(pfd.revents & OS_WAIT_IN))
		return NET_STEP_CONTINUE;
	for (u32 i = 0; i < NET_UDP_MAX_BURST; i++)
	{
		u8		buf[MODBUS_ASCII_MAX_FRAME];
		net_udp_peer_t	peer;
		s32		len = net_udp_recv(&server->udp, buf, sizeof(buf), &peer);

		if (len == 0)
			break;
		if (len > 0)
			handle_datagram(server, &peer, buf, (u32)len);
		else
			net_obs_rejected(server->options.obs);
	}
	return NET_STEP_CONTINUE;
}

void
ascii_udp_server_stop(ascii_udp_server_t* server)
{
	server->stop = 1;
}

s32
ascii_udp_server_init(ascii_udp_server_t* server, const ascii_udp_server_options_t* options)
{
	memset(server, 0, sizeof(*server));
	server->options = *options;
	server->udp.fd  = OS_SOCKET_INVALID;
	rng_seed(&server->rng, options->seed);
	if (net_udp_open(&server->udp, options->bind_address, options->port) < 0)
		return 2;

	LOG_INFO("listening on ASCII-over-UDP %s:%u (%u device%s). Ctrl-C to stop.", options->bind_address,
		 options->port, options->device_count, options->device_count == 1 ? "" : "s");
	return 0;
}

void
ascii_udp_server_close(ascii_udp_server_t* server)
{
	bool was_open = os_socket_valid(server->udp.fd);

	net_udp_close(&server->udp);
	if (was_open)
		LOG_INFO("stopped");
}

s32
ascii_udp_server_run(ascii_udp_server_t* server, const ascii_udp_server_options_t* options)
{
	s32 rc = ascii_udp_server_init(server, options);

	if (rc != 0)
		return rc;
	while (ascii_udp_server_step(server, -1) == NET_STEP_CONTINUE)
		;
	ascii_udp_server_close(server);
	return 0;
}
