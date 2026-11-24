/* modbus-sim: a Modbus TCP device simulator configured with a Scheme file.
 * The vlib arena implementation lives in this file only. */
#include "device/fault.h"
#define ARENA_IMPL
/* Vendored vlib/arena.h: arena_push() dereferences a freshly allocated block
 * without checking arena_alloc()'s NULL return. main.c checks its own
 * ARENA_ALLOC() results; suppress this analyzer diagnostic only around the
 * vendored implementation included here. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-dereference"
#endif
#include "vlib/arena.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "vlib/sizes.h" // IWYU pragma: keep
#include "config/config.h"
#include "net/ascii_server.h"
#include "net/ascii_tcp_server.h"
#include "net/rtu_framer.h"
#include "net/rtu_server.h"
#include "net/rtu_tcp_server.h"
#include "net/tcp_server.h"
#include "util/log.h"
#include "util/serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct cli_t cli_t;
struct cli_t
{
	const char*	config_path;
	bool		check_only;
	bool		verbose;
	bool		show_help;
};

static inline void
print_usage(void)
{
	puts("modbus-sim - Modbus TCP/RTU/ASCII device simulator\n"
	     "\n"
	     "Usage: modbus-sim [options] CONFIG.scm\n"
	     "\n"
	     "Options:\n"
	     "  -c, --check          check the config, print a summary and exit\n"
	     "  -v, --verbose        log every request\n"
	     "  -h, --help           show this help\n"
	     "\n"
	     "Transport/server settings (mode, path, port, bind, serial options) are\n"
	     "defined in (server ...) in the config file; there are no server CLI options.\n"
	     "\n"
	     "Exit status: 0 success, 1 config-load/runtime error, 2 invalid CLI arguments.\n"
	     "\n"
	     "RTU byte streams use a PTY or an existing serial-like device selected by\n"
	     "(server (mode rtu-pty|rtu-device) ...). ASCII uses a byte stream (PTY or\n"
	     "device) selected by (mode ascii-pty|ascii-device), or a TCP listen socket\n"
	     "selected by (mode ascii-tcp); both carry the same raw ASCII frames.\n"
	     "ASCII serial ttys default to 8N1; (data-bits 7) (parity even) (stop-bits 1) selects 7E1.\n"
	     "ASCII frames are ':' + hex + LRC + CRLF, delimited by CRLF, not by timing.\n"
	     "RTU-over-TCP serves Modbus RTU frames on a TCP listen socket (separate transport).\n"
	     "Frame boundaries use Modbus t3.5 when baud is known, or frame-gap-ms/default.\n"
	     "t1.5 discard and unit-0 broadcast execution are opt-in via (server ...).\n"
	     "Modbus TCP may be TLS-wrapped with (server (mode tcp) (tls ...)); it then\n"
	     "defaults to port 802 unless (port N) is given, and requires OpenSSL at build time.\n"
	     "\n"
	     "Try:  modbus-sim profiles/minimal.scm\n"
	     "Docs: README.md and docs/");
}

/* Returns false and prints a message when the arguments make no sense. */
static bool
parse_args(int argc, char** argv, cli_t* cli)
{
	for (int i = 1; i < argc; i++)
	{
		const char* arg = argv[i];

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
			cli->show_help = true;
		else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--check") == 0)
			cli->check_only = true;
		else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)
			cli->verbose = true;
		else if (arg[0] == '-')
		{
			fprintf(stderr, "unknown option '%s' (try --help)\n", arg);
			return false;
		}
		else if (!cli->config_path)
			cli->config_path = arg;
		else
		{
			fprintf(stderr, "unexpected argument '%s' (try --help)\n", arg);
			return false;
		}
	}
	return true;
}

static inline u32
count_points(const device_t* dev, space_kind_t space)
{
	u32	count = 0;

	for (u32 i = 0; i < dev->point_count; i++)
		count += (dev->points[i].space == space);
	return count;
}

static const char*
fault_text(const fault_t* fault, char* buf, u32 size)
{
	switch (fault->kind)
	{
		case FAULT_NONE     : return "none";
		case FAULT_DROP     : snprintf(buf, size, "drop %u%% of responses", fault->percent); break;
		case FAULT_DELAY    : snprintf(buf, size, "delay %u%% of responses by %u ms", fault->percent, fault->delay_ms); break;
		case FAULT_EXCEPTION: snprintf(buf, size, "exception %u on %u%% of responses", fault->exception_code, fault->percent); break;
		case FAULT_CORRUPT  : snprintf(buf, size, "corrupt %u%% of responses", fault->percent); break;
	}

	return buf;
}

static void
print_rtu_timing(const config_t* config)
{
	if (config->frame_gap_ms > 0)
		printf(", frame-gap %u ms (override)", config->frame_gap_ms);
	else if (config->baud > 0)
		printf(", baud %u, t3.5 %u us (frame-gap %u ms)",
		       config->baud, rtu_t35_us(config->baud),
		       rtu_effective_gap_ms(0, config->baud));
	else
		printf(", frame-gap %u ms (default; baud unknown)",
		       rtu_effective_gap_ms(0, 0));
}

static void
print_summary(const config_t* config)
{
	char	fault[96];
	const char*	serial_path = "pty";

	if (config->mode == SERVER_MODE_RTU_DEVICE ||
	    config->mode == SERVER_MODE_ASCII_DEVICE)
		serial_path = config->path;

	switch (config->mode)
	{
		case SERVER_MODE_RTU_TCP:
			printf("%u device%s, RTU-over-TCP %s:%u",
			       config->device_count, config->device_count == 1 ? "" : "s",
			       config->bind_address, config->port);
			print_rtu_timing(config);
			if (config->broadcast_execute)
				printf(", broadcast-execute on");
			putchar('\n');
			break;
		case SERVER_MODE_ASCII_TCP:
			printf("%u device%s, ASCII-over-TCP %s:%u\n",
			       config->device_count, config->device_count == 1 ? "" : "s",
			       config->bind_address, config->port);
			break;
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_ASCII_DEVICE:
			printf("%u device%s, ASCII %s\n",
			       config->device_count, config->device_count == 1 ? "" : "s",
			       serial_path);
			break;
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_RTU_DEVICE:
			printf("%u device%s, RTU %s",
			       config->device_count, config->device_count == 1 ? "" : "s",
			       serial_path);
			print_rtu_timing(config);
			if (config->broadcast_execute)
				printf(", broadcast-execute on");
			putchar('\n');
			break;
		case SERVER_MODE_TCP:
			printf("%u device%s on %s:%u", config->device_count,
			       config->device_count == 1 ? "" : "s",
			       config->bind_address, config->port);
			if (config->tls_enabled)
				printf(" (TLS, client certificates %s)",
				       config->tls_require_client_certificate ? "required" : "optional");
			putchar('\n');
			break;
	}

	for (u32 i = 0; i < config->device_count; i++)
	{
		const device_t*	dev = config->devices[i];

		printf("  unit %-3u %-32s coils %u, discrete %u, input %u, holding %u, fault: %s\n",
		       dev->unit_id, dev->name, count_points(dev, SPACE_COIL),
		       count_points(dev, SPACE_DISCRETE), count_points(dev, SPACE_INPUT),
		       count_points(dev, SPACE_HOLDING), fault_text(&dev->fault, fault, sizeof(fault)));
	}
}


static s32
run_rtu(config_t* config)
{
	char	name[128] = { 0 };
	int	slave_fd  = -1;
	s32	status    = 1;
	int	fd;

	fd = open_serial_fd(config->mode == SERVER_MODE_RTU_PTY, "RTU", config->path,
			    config->baud, 8, config->parity, config->stop_bits,
			    &slave_fd, name, sizeof(name));
	if (fd < 0)
		return 1;
	if (slave_fd >= 0)
	{
		printf("RTU PTY slave: %s\n", name);
		fflush(stdout);
	}
	/* PTY master/slave have no real UART baud; only auto-detect on real tty devices. */
	if (config->baud == 0 && config->mode == SERVER_MODE_RTU_DEVICE)
		config->baud = serial_tty_baud(fd);

	{
		rtu_server_options_t	options = {
			.fd                = fd,
			.seed              = config->seed,
			.frame_gap_ms      = config->frame_gap_ms,
			.baud              = config->baud,
			.strict_t15        = config->strict_t15,
			.broadcast_execute = config->broadcast_execute,
			.devices           = config->devices,
			.device_count      = config->device_count,
		};

		status = rtu_server_run(&options);
	}

	if (slave_fd >= 0)
		close(slave_fd);
	if (fd >= 0)
		close(fd);
	return status;
}

static s32
run_ascii(config_t* config)
{
	char	name[128] = { 0 };
	int	slave_fd  = -1;
	s32	status    = 1;
	int	fd;

	/* Baud sets only the serial line speed here; it never affects ASCII framing. */
	fd = open_serial_fd(config->mode == SERVER_MODE_ASCII_PTY, "ASCII", config->path,
			    config->baud, config->data_bits, config->parity, config->stop_bits,
			    &slave_fd, name, sizeof(name));
	if (fd < 0)
		return 1;
	if (slave_fd >= 0)
	{
		printf("ASCII PTY slave: %s\n", name);
		fflush(stdout);
	}

	{
		ascii_server_options_t	options = {
			.fd           = fd,
			.seed         = config->seed,
			.devices      = config->devices,
			.device_count = config->device_count,
		};

		status = ascii_server_run(&options);
	}

	if (slave_fd >= 0)
		close(slave_fd);
	if (fd >= 0)
		close(fd);
	return status;
}

static s32
run_ascii_tcp(config_t* config)
{
	ascii_tcp_server_options_t	options = {
		.bind_address    = config->bind_address,
		.port            = config->port,
		.seed            = config->seed,
		.idle_timeout_ms = config->idle_timeout_ms,
		.devices         = config->devices,
		.device_count    = config->device_count,
	};

	return ascii_tcp_server_run(&options);
}

static s32
run_rtu_tcp(config_t* config)
{
	rtu_tcp_server_options_t	options = {
		.bind_address      = config->bind_address,
		.port              = config->port,
		.seed              = config->seed,
		.idle_timeout_ms   = config->idle_timeout_ms,
		.frame_gap_ms      = config->frame_gap_ms,
		.baud              = config->baud,
		.broadcast_execute = config->broadcast_execute,
		.devices           = config->devices,
		.device_count      = config->device_count,
	};

	return rtu_tcp_server_run(&options);
}

static s32
run_tcp(config_t* config)
{
	tcp_server_options_t	options = {
		.bind_address    = config->bind_address,
		.port            = config->port,
		.seed            = config->seed,
		.idle_timeout_ms = config->idle_timeout_ms,
		.tls_enabled     = config->tls_enabled,
		.tls_handshake_timeout_ms = config->tls_handshake_timeout_ms,
		.tls_certificate = config->tls_certificate,
		.tls_private_key = config->tls_private_key,
		.tls_ca_certificate = config->tls_ca_certificate,
		.tls_require_client_certificate = config->tls_require_client_certificate,
		.tls_min_version = config->tls_min_version == CONFIG_TLS_VERSION_1_3 ?
				   NET_TLS_VERSION_1_3 : NET_TLS_VERSION_1_2,
		.tls1_2_ciphers  = config->tls1_2_ciphers[0] ? config->tls1_2_ciphers : NULL,
		.tls1_3_ciphersuites = config->tls1_3_ciphersuites[0] ?
				       config->tls1_3_ciphersuites : NULL,
		.devices         = config->devices,
		.device_count    = config->device_count,
	};

	return tcp_server_run(&options);
}

int
main(int argc, char** argv)
{
	cli_t		cli     = { 0 };
	config_t	config  = { 0 };
	char		error[512];
	arena_t*	app     = NULL;
	arena_t*	scratch = NULL;
	s32		status  = 1;

	if (!parse_args(argc, argv, &cli))
		return 2;

	if (cli.show_help || !cli.config_path)
	{
		print_usage();
		return cli.show_help ? 0 : 2;
	}

	log_set_verbose(cli.verbose);
	app = ARENA_ALLOC();
	if (!app)
	{
		fprintf(stderr, "out of memory allocating app arena\n");
		return 1;
	}
	scratch = ARENA_ALLOC();
	if (!scratch)
	{
		fprintf(stderr, "out of memory allocating scratch arena\n");
		goto cleanup;
	}

	if (!config_load(app, scratch, cli.config_path, &config, error, sizeof(error)))
	{
		fprintf(stderr, "%s: %s\n", cli.config_path, error);
		goto cleanup;
	}
	arena_release(scratch);
	scratch = NULL;

	print_summary(&config);
	if (cli.check_only)
	{
		puts("config OK");
		status = 0;
		goto cleanup;
	}

	switch (config.mode)
	{
		case SERVER_MODE_TCP:
			status = run_tcp(&config);
			break;
		case SERVER_MODE_RTU_TCP:
			status = run_rtu_tcp(&config);
			break;
		case SERVER_MODE_ASCII_TCP:
			status = run_ascii_tcp(&config);
			break;
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_RTU_DEVICE:
			status = run_rtu(&config);
			break;
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_ASCII_DEVICE:
			status = run_ascii(&config);
			break;
	}

cleanup:
	if (scratch)
		arena_release(scratch);
	if (app)
		arena_release(app);
	return status;
}
