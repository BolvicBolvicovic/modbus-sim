/* mogbus: a Modbus TCP device simulator configured with a Scheme file.
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
#include "net/rtu_framer.h"
#include "server/server.h"
#include "server/server_config.h"
#include "util/log.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The binary owns signal handling; the handler stops the running server. */
static server_t* volatile	g_server;

static void
on_stop_signal(int signo)
{
	server_t* server = g_server;

	(void)signo;
	if (server)
		server_stop(server);
}

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
	puts("mogbus - Modbus TCP/RTU/ASCII device simulator\n"
	     "\n"
	     "Usage: mogbus [options] CONFIG.scm\n"
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
	     "Try:  mogbus profiles/minimal.scm\n"
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
fault_text(const fault_set_t* set, char* buf, u32 size)
{
	u32 used = 0;

	buf[0] = '\0';
	for (u32 i = 0; i < FAULT_KIND_COUNT; i++)
	{
		char	part[80];
		char	trigger[40];
		u32	written;

		if ((set->present & FAULT_BIT(i)) == 0)
			continue;
		if (set->by_kind[i].schedule_id != 0)
			snprintf(trigger, sizeof(trigger), "by schedule");
		else
			snprintf(trigger, sizeof(trigger), "%u%% of responses", set->by_kind[i].percent);
		switch ((fault_kind_t)i)
		{
			case FAULT_DROP       : snprintf(part, sizeof(part), "drop %s", trigger); break;
			case FAULT_DELAY      : snprintf(part, sizeof(part), "delay %s by %u ms",
							 trigger, set->by_kind[i].delay_ms); break;
			case FAULT_EXCEPTION  : snprintf(part, sizeof(part), "exception %u on %s",
							 set->by_kind[i].exception_code, trigger); break;
			case FAULT_CORRUPT    : snprintf(part, sizeof(part), "corrupt %s", trigger); break;
			case FAULT_DISCONNECT : snprintf(part, sizeof(part), "disconnect %s", trigger); break;
			case FAULT_RESTART    : snprintf(part, sizeof(part), "restart %s", trigger); break;
			case FAULT_SENSOR_DISCONNECT :
				snprintf(part, sizeof(part), "sensor-disconnect %s", trigger); break;
			default: continue;
		}
		written = (u32)snprintf(buf + used, size - used, "%s%s",
					used ? "; " : "", part);
		if (written >= size - used)
			break;
		used += written;
	}
	return used == 0 ? "none" : buf;
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
	char	fault[512];
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
		case SERVER_MODE_RTU_FD:
		case SERVER_MODE_ASCII_FD:
			break;	/* host-only, never loaded from config */
	}

	for (u32 i = 0; i < config->device_count; i++)
	{
		const device_t*	dev = config->devices[i];

		printf("  unit %-3u %-32s coils %u, discrete %u, input %u, holding %u, fault: %s\n",
		       dev->unit_id, dev->name, count_points(dev, SPACE_COIL),
		       count_points(dev, SPACE_DISCRETE), count_points(dev, SPACE_INPUT),
		       count_points(dev, SPACE_HOLDING), fault_text(&dev->faults, fault, sizeof(fault)));
	}
}


int
main(int argc, char** argv)
{
	cli_t		cli     = { 0 };
	config_t	config  = { 0 };
	server_options_t options;
	server_t	server;
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

	log_set_level(cli.verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
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
	options = server_options_from_config(&config);
	if (cli.check_only)
	{
		if (!server_check_tls(&options, error, sizeof(error)))
		{
			fprintf(stderr, "%s: cannot configure TLS: %s\n", cli.config_path, error);
			goto cleanup;
		}
		puts("config OK");
		status = 0;
		goto cleanup;
	}

	{
		struct sigaction sa = { 0 };

		sa.sa_handler = on_stop_signal;	/* no SA_RESTART: poll() must wake up */
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		signal(SIGPIPE, SIG_IGN);
	}

	status = server_init(&server, &options);
	if (status != 0)
		goto cleanup;
	g_server = &server;
	if (server_pty_name(&server))
	{
		printf("%s PTY slave: %s\n", config.mode == SERVER_MODE_RTU_PTY ? "RTU" : "ASCII",
		       server_pty_name(&server));
		fflush(stdout);
	}
	status = server_serve(&server);
	g_server = NULL;

cleanup:
	if (scratch)
		arena_release(scratch);
	if (app)
		arena_release(app);
	return status;
}
