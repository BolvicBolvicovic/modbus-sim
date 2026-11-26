/**
 * @file
 * @brief Modbus UDP server: one MBAP ADU per datagram.
 */
#define _GNU_SOURCE
#include "net/udp_server.h"
#include "modbus/pdu.h"
#include "modbus/mbap.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "net/net_udp.h"
#include "util/clock.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <poll.h>
#include <string.h>

/** @brief One datagram is one complete ADU: the MBAP length must account for every byte. */
static void
handle_datagram(udp_server_t* server, const net_udp_peer_t* peer, const u8* buf, u32 len)
{
	mbap_header_t		header   = { 0 };
	u8			out[MBAP_MAX_FRAME];
	fault_decision_t	decision = { 0 };
	u64			now_ms   = clock_now_ms();
	u32			out_len;

	if (len >= MBAP_HEADER_SIZE)
		mbap_decode(buf, &header);
	if (len < MBAP_HEADER_SIZE + 1 ||
	    !mbap_is_valid(&header) ||
	    mbap_frame_size(&header) != len)
	{
		LOG_DEBUG("UDP %s sent a malformed MBAP datagram (%u bytes), ignored", peer->text, len);
		net_obs_rejected(server->options.obs);
		return;
	}

	out_len = net_mbap_respond(server->options.obs, server->options.devices, server->options.device_count,
				   &server->rng, now_ms, peer->text, &header,
				   buf + MBAP_HEADER_SIZE, out, &decision);
	if (out_len == 0)
		return;
	(void)net_udp_send_at(&server->udp, peer, out, out_len, now_ms + decision.delay_ms, now_ms);
}

s32
udp_server_step(udp_server_t* server, s32 max_wait_ms)
{
	struct pollfd	pfd     = { .fd = server->udp.fd, .events = POLLIN };
	int		timeout = 1000;
	u64		now_ms  = clock_now_ms();

	if (server->stop)
		return NET_STEP_DONE;

	net_udp_flush_due(&server->udp, now_ms);
	net_udp_poll_timeout(&server->udp, now_ms, &timeout);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (net_poll(server->options.obs, &pfd, 1, timeout) < 0)
		return NET_STEP_CONTINUE;	/* EINTR: the caller re-checks stop */

	net_udp_flush_due(&server->udp, clock_now_ms());
	if (!(pfd.revents & POLLIN))
		return NET_STEP_CONTINUE;
	for (u32 i = 0; i < NET_UDP_MAX_BURST; i++)
	{
		u8		buf[MBAP_MAX_FRAME];
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
udp_server_stop(udp_server_t* server)
{
	server->stop = 1;
}

s32
udp_server_init(udp_server_t* server, const udp_server_options_t* options)
{
	memset(server, 0, sizeof(*server));
	server->options = *options;
	server->udp.fd  = -1;
	rng_seed(&server->rng, options->seed);
	if (net_udp_open(&server->udp, options->bind_address, options->port) < 0)
		return 2;

	LOG_INFO("listening on UDP %s:%u (%u device%s). Ctrl-C to stop.", options->bind_address,
		 options->port, options->device_count, options->device_count == 1 ? "" : "s");
	return 0;
}

void
udp_server_close(udp_server_t* server)
{
	bool was_open = server->udp.fd >= 0;

	net_udp_close(&server->udp);
	if (was_open)
		LOG_INFO("stopped");
}

s32
udp_server_run(udp_server_t* server, const udp_server_options_t* options)
{
	s32 rc = udp_server_init(server, options);

	if (rc != 0)
		return rc;
	while (udp_server_step(server, -1) == NET_STEP_CONTINUE)
		;
	udp_server_close(server);
	return 0;
}
