/**
 * @file
 * @brief Transport-independent server facade: opens PTYs and serial devices and drives the selected transport.
 */
#include "server/server.h"
#include "os/os_error.h"
#include "os/os_wait.h"
#include "util/log.h"
#include <stdio.h>
#include <string.h>

_Static_assert((int)NET_OBS_MAX_DEVICES == (int)SERVER_MAX_DEVICES, "metrics keep one slot per device");

static const char* const MODE_NAMES[] = {
	[SERVER_MODE_TCP]          = "tcp",
	[SERVER_MODE_RTU_PTY]      = "rtu-pty",
	[SERVER_MODE_RTU_DEVICE]   = "rtu-device",
	[SERVER_MODE_RTU_TCP]      = "rtu-tcp",
	[SERVER_MODE_ASCII_PTY]    = "ascii-pty",
	[SERVER_MODE_ASCII_DEVICE] = "ascii-device",
	[SERVER_MODE_ASCII_TCP]    = "ascii-tcp",
	[SERVER_MODE_RTU_FD]       = "rtu-fd",
	[SERVER_MODE_ASCII_FD]     = "ascii-fd",
	[SERVER_MODE_UDP]          = "udp",
	[SERVER_MODE_ASCII_UDP]    = "ascii-udp",
	[SERVER_MODE_RTU_UDP]      = "rtu-udp",
};

static s32
server_open(server_t* server, const server_options_t* options)
{
	s32 rc = 0;
	int fd = options->fd;

	memset(server, 0, sizeof(*server));
	if (options->id)
		snprintf(server->id, sizeof(server->id), "%s", options->id);
	log_set_prefix(server->id);
	server->mode     = options->mode;
	server->fd       = -1;
	server->slave_fd = -1;
	/* The endpoint opens first so a port clash fails start-up like a transport listener. */
	if (net_obs_init(&server->obs, MODE_NAMES[options->mode], options->devices,
			 options->device_count, options->http_bind_address, options->http_port) != 0)
	{
		net_obs_close(&server->obs);
		return 2;
	}
	if ((options->http_control || options->http_ui) && options->http_bind_address)
	{
		server->obs.control.enabled  = true;
		server->obs.control.ui       = options->http_ui;
		if (options->http_allowed_hosts)
			snprintf(server->obs.control.allowed_hosts, sizeof(server->obs.control.allowed_hosts), "%s",
				 options->http_allowed_hosts);
		server->obs.control.tcp_like = options->mode == SERVER_MODE_TCP ||
					       options->mode == SERVER_MODE_RTU_TCP ||
					       options->mode == SERVER_MODE_ASCII_TCP;
		server->obs.control.mbap     = options->mode == SERVER_MODE_TCP ||
					       options->mode == SERVER_MODE_UDP;
		LOG_WARN("HTTP control API enabled on %s:%u: anyone who can reach this port can change "
			 "device state and faults", options->http_bind_address, options->http_port);
	}

	switch (options->mode)
	{
		case SERVER_MODE_TCP:
		{
			tcp_server_options_t o = {
				.bind_address             = options->bind_address,
				.port                     = options->port,
				.seed                     = options->seed,
				.idle_timeout_ms          = options->idle_timeout_ms,
				.tls_enabled              = options->tls_enabled,
				.tls_handshake_timeout_ms = options->tls_handshake_timeout_ms,
				.tls                      = options->tls,
				.devices                  = options->devices,
				.device_count             = options->device_count,
				.obs                      = &server->obs,
			};

			rc = tcp_server_init(&server->tcp, &o);
			break;
		}
		case SERVER_MODE_UDP:
		{
			udp_server_options_t o = {
				.bind_address = options->bind_address,
				.port         = options->port,
				.seed         = options->seed,
				.devices      = options->devices,
				.device_count = options->device_count,
				.obs          = &server->obs,
			};

			rc = udp_server_init(&server->udp, &o);
			break;
		}
		case SERVER_MODE_ASCII_UDP:
		{
			ascii_udp_server_options_t o = {
				.bind_address = options->bind_address,
				.port         = options->port,
				.seed         = options->seed,
				.devices      = options->devices,
				.device_count = options->device_count,
				.obs          = &server->obs,
			};

			rc = ascii_udp_server_init(&server->ascii_udp, &o);
			break;
		}
		case SERVER_MODE_RTU_UDP:
		{
			rtu_udp_server_options_t o = {
				.bind_address      = options->bind_address,
				.port              = options->port,
				.seed              = options->seed,
				.broadcast_execute = options->broadcast_execute,
				.devices           = options->devices,
				.device_count      = options->device_count,
				.obs               = &server->obs,
			};

			rc = rtu_udp_server_init(&server->rtu_udp, &o);
			break;
		}
		case SERVER_MODE_RTU_TCP:
		{
			rtu_tcp_server_options_t o = {
				.bind_address      = options->bind_address,
				.port              = options->port,
				.seed              = options->seed,
				.idle_timeout_ms   = options->idle_timeout_ms,
				.frame_gap_ms      = options->frame_gap_ms,
				.baud              = options->baud,
				.broadcast_execute = options->broadcast_execute,
				.devices           = options->devices,
				.device_count      = options->device_count,
				.obs               = &server->obs,
			};

			rc = rtu_tcp_server_init(&server->rtu_tcp, &o);
			break;
		}
		case SERVER_MODE_ASCII_TCP:
		{
			ascii_tcp_server_options_t o = {
				.bind_address    = options->bind_address,
				.port            = options->port,
				.seed            = options->seed,
				.idle_timeout_ms = options->idle_timeout_ms,
				.devices         = options->devices,
				.device_count    = options->device_count,
				.obs             = &server->obs,
			};

			rc = ascii_tcp_server_init(&server->ascii_tcp, &o);
			break;
		}
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_RTU_DEVICE:
		case SERVER_MODE_RTU_FD:
		{
			rtu_server_options_t o = {
				.seed              = options->seed,
				.frame_gap_ms      = options->frame_gap_ms,
				.baud              = options->baud,
				.strict_t15        = options->strict_t15,
				.char_timing       = options->char_timing,
				.broadcast_execute = options->broadcast_execute,
				.devices           = options->devices,
				.device_count      = options->device_count,
				.obs               = &server->obs,
			};

			if (options->mode != SERVER_MODE_RTU_FD)
			{
				fd = os_serial_open(options->mode == SERVER_MODE_RTU_PTY, "RTU",
						    options->path, options->baud, 8, options->parity,
						    options->stop_bits, &server->slave_fd,
						    server->pty_name, sizeof(server->pty_name));
				if (fd < 0)
					return 1;
				server->fd = fd;
				/* PTY master/slave have no real UART baud; only auto-detect on real tty devices. */
				if (o.baud == 0 && options->mode == SERVER_MODE_RTU_DEVICE)
					o.baud = os_serial_baud(fd);
				if (o.char_timing &&
				    options->mode == SERVER_MODE_RTU_DEVICE &&
				    !os_serial_low_latency(fd))
					LOG_WARN("RTU: no low-latency mode on %s; characters are dated as late as the driver delivers them",
						 options->path);
			}
			else if (!os_serial_supported())
			{
				fprintf(stderr, "RTU fd mode is not supported on this platform\n");
				return 1;
			}
			if (o.char_timing &&
			    o.baud == 0)
			{
				fprintf(stderr, "RTU: character timing needs a known baud rate\n");
				return 1;
			}
			o.fd = fd;
			rc = rtu_server_init(&server->rtu, &o);
			break;
		}
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_ASCII_DEVICE:
		case SERVER_MODE_ASCII_FD:
		{
			ascii_server_options_t o = {
				.seed         = options->seed,
				.devices      = options->devices,
				.device_count = options->device_count,
				.obs          = &server->obs,
			};

			if (options->mode != SERVER_MODE_ASCII_FD)
			{
				/* Baud sets only the serial line speed here; it never affects ASCII framing. */
				fd = os_serial_open(options->mode == SERVER_MODE_ASCII_PTY, "ASCII",
						    options->path, options->baud, options->data_bits,
						    options->parity, options->stop_bits, &server->slave_fd,
						    server->pty_name, sizeof(server->pty_name));
				if (fd < 0)
					return 1;
				server->fd = fd;
			}
			else if (!os_serial_supported())
			{
				fprintf(stderr, "ASCII fd mode is not supported on this platform\n");
				return 1;
			}
			o.fd = fd;
			rc = ascii_server_init(&server->ascii, &o);
			break;
		}
	}
	if (rc != 0)
		server_close(server);
	else
		server->obs.ready = true;
	return rc;
}

s32
server_init(server_t* server, const server_options_t* options)
{
	s32 rc = server_open(server, options);

	log_set_prefix(NULL);
	return rc;
}

static s32
step_transport(server_t* server, s32 max_wait_ms)
{
	switch (server->mode)
	{
		case SERVER_MODE_TCP:		return tcp_server_step(&server->tcp, max_wait_ms);
		case SERVER_MODE_UDP:		return udp_server_step(&server->udp, max_wait_ms);
		case SERVER_MODE_ASCII_UDP:	return ascii_udp_server_step(&server->ascii_udp, max_wait_ms);
		case SERVER_MODE_RTU_UDP:	return rtu_udp_server_step(&server->rtu_udp, max_wait_ms);
		case SERVER_MODE_RTU_TCP:	return rtu_tcp_server_step(&server->rtu_tcp, max_wait_ms);
		case SERVER_MODE_ASCII_TCP:	return ascii_tcp_server_step(&server->ascii_tcp, max_wait_ms);
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_RTU_DEVICE:
		case SERVER_MODE_RTU_FD:	return rtu_server_step(&server->rtu, max_wait_ms);
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_ASCII_DEVICE:
		case SERVER_MODE_ASCII_FD:	return ascii_server_step(&server->ascii, max_wait_ms);
	}
	return NET_STEP_ERROR;
}

s32
server_step(server_t* server, s32 max_wait_ms)
{
	s32 rc;

	log_set_prefix(server->id);
	rc = step_transport(server, max_wait_ms);
	log_set_prefix(NULL);
	return rc;
}

s32
server_group_step(server_t** servers, u32 count, net_poll_group_t* group, s32 max_wait_ms)
{
	u32	live   = 0;
	s32	result = NET_STEP_CONTINUE;
	int	timeout;
	int	rc;

	group->phase      = NET_POLL_GATHER;
	group->count      = 0;
	group->timeout_ms = -1;
	for (u32 i = 0; i < count; i++)
	{
		server_t* s = servers[i];

		if (!s ||
		    s->done)
			continue;
		s->obs.group = group;
		switch (server_step(s, max_wait_ms))
		{
			case NET_STEP_DONE:	s->done = true; break;
			case NET_STEP_ERROR:	result = NET_STEP_ERROR; break;
			default:		live++; break;
		}
	}
	if (live > 0 &&
	    result == NET_STEP_CONTINUE)
	{
		timeout = group->timeout_ms < 0 ? 1000 : group->timeout_ms;
		if (max_wait_ms >= 0 &&
		    max_wait_ms < timeout)
			timeout = max_wait_ms;
		if (group->lock)
			os_mutex_unlock(group->lock);
		rc = os_wait(group->entries, group->count, timeout);
		if (group->lock)
			os_mutex_lock(group->lock);
		if (rc < 0 &&
		    !os_interrupted())
		{
			LOG_WARN("poll failed: %s", os_last_error_text());
			result = NET_STEP_ERROR;
		}
		else if (rc >= 0)
		{
			/* Each step runs again and reads the revents of its entries. */
			group->phase = NET_POLL_REPLAY;
			live         = 0;
			for (u32 i = 0; i < count; i++)
			{
				server_t* s = servers[i];

				if (!s ||
				    s->done)
					continue;
				switch (server_step(s, 0))
				{
					case NET_STEP_DONE:	s->done = true; break;
					case NET_STEP_ERROR:	result = NET_STEP_ERROR; break;
					default:		live++; break;
				}
			}
		}
	}
	for (u32 i = 0; i < count; i++)
	{
		if (servers[i])
			servers[i]->obs.group = NULL;
	}
	if (result == NET_STEP_ERROR)
		return result;
	for (u32 i = 0; i < count; i++)
	{
		if (servers[i] &&
		    !servers[i]->done)
			return NET_STEP_CONTINUE;
	}
	return NET_STEP_DONE;
}

void
server_stop(server_t* server)
{
	server->obs.ready = false;
	switch (server->mode)
	{
		case SERVER_MODE_TCP:		tcp_server_stop(&server->tcp); break;
		case SERVER_MODE_UDP:		udp_server_stop(&server->udp); break;
		case SERVER_MODE_ASCII_UDP:	ascii_udp_server_stop(&server->ascii_udp); break;
		case SERVER_MODE_RTU_UDP:	rtu_udp_server_stop(&server->rtu_udp); break;
		case SERVER_MODE_RTU_TCP:	rtu_tcp_server_stop(&server->rtu_tcp); break;
		case SERVER_MODE_ASCII_TCP:	ascii_tcp_server_stop(&server->ascii_tcp); break;
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_RTU_DEVICE:
		case SERVER_MODE_RTU_FD:	rtu_server_stop(&server->rtu); break;
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_ASCII_DEVICE:
		case SERVER_MODE_ASCII_FD:	ascii_server_stop(&server->ascii); break;
	}
}

void
server_close(server_t* server)
{
	log_set_prefix(server->id);
	switch (server->mode)
	{
		case SERVER_MODE_TCP:		tcp_server_close(&server->tcp); break;
		case SERVER_MODE_UDP:		udp_server_close(&server->udp); break;
		case SERVER_MODE_ASCII_UDP:	ascii_udp_server_close(&server->ascii_udp); break;
		case SERVER_MODE_RTU_UDP:	rtu_udp_server_close(&server->rtu_udp); break;
		case SERVER_MODE_RTU_TCP:	rtu_tcp_server_close(&server->rtu_tcp); break;
		case SERVER_MODE_ASCII_TCP:	ascii_tcp_server_close(&server->ascii_tcp); break;
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_RTU_DEVICE:
		case SERVER_MODE_RTU_FD:	rtu_server_close(&server->rtu); break;
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_ASCII_DEVICE:
		case SERVER_MODE_ASCII_FD:	ascii_server_close(&server->ascii); break;
	}
	net_obs_close(&server->obs);
	if (server->slave_fd >= 0)
		os_serial_close(server->slave_fd);
	if (server->fd >= 0)
		os_serial_close(server->fd);
	server->slave_fd = -1;
	server->fd       = -1;
	log_set_prefix(NULL);
}

const char*
server_pty_name(const server_t* server)
{
	return server->slave_fd >= 0 ? server->pty_name : NULL;
}

s32
server_serve(server_t* server)
{
	s32 rc;

	while ((rc = server_step(server, -1)) == NET_STEP_CONTINUE)
		;
	server_close(server);
	return rc == NET_STEP_ERROR ? 1 : 0;
}

s32
server_run(server_t* server, const server_options_t* options)
{
	s32 rc = server_init(server, options);

	return rc != 0 ? rc : server_serve(server);
}

bool
server_check_tls(const server_options_t* options, net_tls_ctx_t* tls)
{
	if (!options->tls_enabled)
		return true;
	memset(tls, 0, sizeof(*tls));
	if (!net_tls_ctx_init(tls, &options->tls))
		return false;
	net_tls_ctx_close(tls);
	return true;
}
