/**
 * @file
 * @brief Modbus RTU over UDP: one frame per datagram.
 */
#define _GNU_SOURCE
#include "net/rtu_udp_server.h"
#include "modbus/rtu.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "net/net_udp.h"
#include "net/rtu_framer.h"
#include "util/clock.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <poll.h>
#include <string.h>

/**
 * @brief One datagram is one complete RTU frame: anything else (bad CRC, extra bytes, several
 *        frames) is rejected whole and counted like a bad frame on rtu-tcp.
 */
static void
handle_datagram(rtu_udp_server_t* server, const net_udp_peer_t* peer, const u8* buf, u32 len)
{
	rtu_framer_t*		framer    = &server->framer;
	rtu_framer_ctx_t	ctx       = {
		.devices           = server->options.devices,
		.device_count      = server->options.device_count,
		.rng               = &server->rng,
		.broadcast_execute = server->options.broadcast_execute,
		.peer              = peer->text,
		.obs               = server->options.obs,
	};
	u32			frame_len = 0;
	u64			now_ms    = clock_now_ms();

	if (modbus_rtu_next_frame(buf, len, &frame_len) != MODBUS_RTU_FRAME_FOUND ||
	    frame_len != len)
	{
		LOG_DEBUG("UDP %s sent a malformed RTU datagram (%u bytes), ignored", peer->text, len);
		if (len >= 4)
			net_record_comm_error(ctx.obs, ctx.devices, ctx.device_count);
		else
			net_obs_rejected(ctx.obs);
		return;
	}

	pending_response_clear(&framer->pending);
	rtu_framer_handle_frame(framer, &ctx, now_ms, buf, len);
	if (!framer->pending.active)
		return;
	(void)net_udp_send_at(&server->udp, peer, framer->pending.buf, framer->pending.len,
			      framer->pending.due_ms, now_ms);
	pending_response_clear(&framer->pending);
}

s32
rtu_udp_server_step(rtu_udp_server_t* server, s32 max_wait_ms)
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
		u8		buf[MODBUS_RTU_MAX_FRAME];
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
rtu_udp_server_stop(rtu_udp_server_t* server)
{
	server->stop = 1;
}

s32
rtu_udp_server_init(rtu_udp_server_t* server, const rtu_udp_server_options_t* options)
{
	memset(server, 0, sizeof(*server));
	server->options = *options;
	server->udp.fd  = -1;
	rtu_framer_init(&server->framer);
	rng_seed(&server->rng, options->seed);
	if (net_udp_open(&server->udp, options->bind_address, options->port) < 0)
		return 2;

	LOG_INFO("RTU-over-UDP listening on %s:%u (%u device%s). Ctrl-C to stop.", options->bind_address,
		 options->port, options->device_count, options->device_count == 1 ? "" : "s");
	return 0;
}

void
rtu_udp_server_close(rtu_udp_server_t* server)
{
	bool was_open = server->udp.fd >= 0;

	net_udp_close(&server->udp);
	if (was_open)
		LOG_INFO("RTU-over-UDP stopped");
}

s32
rtu_udp_server_run(rtu_udp_server_t* server, const rtu_udp_server_options_t* options)
{
	s32 rc = rtu_udp_server_init(server, options);

	if (rc != 0)
		return rc;
	while (rtu_udp_server_step(server, -1) == NET_STEP_CONTINUE)
		;
	rtu_udp_server_close(server);
	return 0;
}
