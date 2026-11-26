#include "server/server.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

s32
server_init(server_t* server, const server_options_t* options)
{
	s32 rc = 0;
	int fd = options->fd;

	memset(server, 0, sizeof(*server));
	server->mode     = options->mode;
	server->fd       = -1;
	server->slave_fd = -1;

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
			};

			return tcp_server_init(&server->tcp, &o);
		}
		case SERVER_MODE_UDP:
		{
			udp_server_options_t o = {
				.bind_address = options->bind_address,
				.port         = options->port,
				.seed         = options->seed,
				.devices      = options->devices,
				.device_count = options->device_count,
			};

			return udp_server_init(&server->udp, &o);
		}
		case SERVER_MODE_ASCII_UDP:
		{
			ascii_udp_server_options_t o = {
				.bind_address = options->bind_address,
				.port         = options->port,
				.seed         = options->seed,
				.devices      = options->devices,
				.device_count = options->device_count,
			};

			return ascii_udp_server_init(&server->ascii_udp, &o);
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
			};

			return rtu_udp_server_init(&server->rtu_udp, &o);
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
			};

			return rtu_tcp_server_init(&server->rtu_tcp, &o);
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
			};

			return ascii_tcp_server_init(&server->ascii_tcp, &o);
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
				.broadcast_execute = options->broadcast_execute,
				.devices           = options->devices,
				.device_count      = options->device_count,
			};

			if (options->mode != SERVER_MODE_RTU_FD)
			{
				fd = open_serial_fd(options->mode == SERVER_MODE_RTU_PTY, "RTU",
						    options->path, options->baud, 8, options->parity,
						    options->stop_bits, &server->slave_fd,
						    server->pty_name, sizeof(server->pty_name));
				if (fd < 0)
					return 1;
				server->fd = fd;
				/* PTY master/slave have no real UART baud; only auto-detect on real tty devices. */
				if (o.baud == 0 && options->mode == SERVER_MODE_RTU_DEVICE)
					o.baud = serial_tty_baud(fd);
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
			};

			if (options->mode != SERVER_MODE_ASCII_FD)
			{
				/* Baud sets only the serial line speed here; it never affects ASCII framing. */
				fd = open_serial_fd(options->mode == SERVER_MODE_ASCII_PTY, "ASCII",
						    options->path, options->baud, options->data_bits,
						    options->parity, options->stop_bits, &server->slave_fd,
						    server->pty_name, sizeof(server->pty_name));
				if (fd < 0)
					return 1;
				server->fd = fd;
			}
			o.fd = fd;
			rc = ascii_server_init(&server->ascii, &o);
			break;
		}
	}
	if (rc != 0)
		server_close(server);
	return rc;
}

s32
server_step(server_t* server, s32 max_wait_ms)
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

void
server_stop(server_t* server)
{
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
	if (server->slave_fd >= 0)
		close(server->slave_fd);
	if (server->fd >= 0)
		close(server->fd);
	server->slave_fd = -1;
	server->fd       = -1;
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
server_check_tls(const server_options_t* options, char* err, u32 err_size)
{
	net_tls_ctx_t ctx;

	if (!options->tls_enabled)
		return true;
	memset(&ctx, 0, sizeof(ctx));
	if (!net_tls_ctx_init(&ctx, &options->tls, err, err_size))
		return false;
	net_tls_ctx_close(&ctx);
	return true;
}
