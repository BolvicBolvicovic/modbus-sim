/**
 * @file
 * @brief mogbus: a Modbus device simulator configured with a Scheme file.
 *
 * A pure client of the public library API (mogbus.h); nothing here touches an internal module.
 */
#include "mogbus.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief The binary owns signal handling; the handler stops the running server. */
static mogbus_server_t* volatile	g_server;

static void
on_stop_signal(int signo)
{
	mogbus_server_t* server = g_server;

	(void)signo;
	if (server)
		mogbus_server_stop(server);
}

typedef struct cli_t cli_t;
struct cli_t
{
	const char*	config_path;
	bool		check_only;
	bool		health;
	uint32_t	timeout_ms;
	bool		verbose;
	bool		show_help;
	bool		show_version;
	bool		log_json;
};

static inline void
print_usage(void)
{
	puts("mogbus - Modbus TCP/UDP/RTU/ASCII device simulator\n"
	     "\n"
	     "Usage: mogbus [options] CONFIG.scm\n"
	     "\n"
	     "Options:\n"
	     "  -c, --check          check the config, print a summary and exit\n"
	     "      --health         probe the server the config describes (FC 08 echo to its first\n"
	     "                       device) and exit 0 if it answers, 1 if not\n"
	     "      --timeout-ms N   --health reply timeout (default 2000)\n"
	     "  -v, --verbose        log every request\n"
	     "      --log-format F   log lines as text (default) or json\n"
	     "  -V, --version        print the version and exit\n"
	     "  -h, --help           show this help\n"
	     "\n"
	     "Transport/server settings (mode, path, port, bind, serial options) are\n"
	     "defined in (server ...) in the config file; there are no server CLI options.\n"
	     "\n"
	     "Exit status: 0 success, 1 config/serial/runtime error, 2 invalid CLI arguments\n"
	     "or listener/TLS start-up failure.\n"
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
	     "(mode udp) serves Modbus UDP: one MBAP ADU per datagram on a UDP socket.\n"
	     "(mode ascii-udp) carries one ASCII frame (:...CR LF) per UDP datagram.\n"
	     "(mode rtu-udp) carries one RTU frame per UDP datagram (no frame-gap timing).\n"
	     "Modbus TCP may be TLS-wrapped with (server (mode tcp) (tls ...)); it then\n"
	     "defaults to port 802 unless (port N) is given, and requires OpenSSL at build time.\n"
	     "\n"
	     "Try:  mogbus profiles/minimal.scm\n"
	     "Docs: README.md and docs/user/");
}

/** @brief Returns false and prints a message when the arguments make no sense. */
static bool
parse_args(int argc, char** argv, cli_t* cli)
{
	for (int i = 1; i < argc; i++)
	{
		const char* arg = argv[i];

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
			cli->show_help = true;
		else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0)
			cli->show_version = true;
		else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--check") == 0)
			cli->check_only = true;
		else if (strcmp(arg, "--health") == 0)
			cli->health = true;
		else if (strcmp(arg, "--timeout-ms") == 0)
		{
			char* end = NULL;
			long  value = i + 1 < argc ? strtol(argv[i + 1], &end, 10) : 0;

			if (i + 1 >= argc || end == argv[i + 1] || *end != '\0' || value < 1 || value > 600000)
			{
				fprintf(stderr, "--timeout-ms needs a number of milliseconds (1-600000)\n");
				return false;
			}
			cli->timeout_ms = (uint32_t)value;
			i++;
		}
		else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)
			cli->verbose = true;
		else if (strcmp(arg, "--log-format") == 0)
		{
			const char* value = i + 1 < argc ? argv[++i] : "";

			if (strcmp(value, "json") == 0)
				cli->log_json = true;
			else if (strcmp(value, "text") != 0)
			{
				fprintf(stderr, "--log-format needs text or json\n");
				return false;
			}
		}
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

static const char*
fault_text(const mogbus_device_info_t* info, char* buf, size_t size)
{
	size_t used = 0;

	buf[0] = '\0';
	for (int i = 0; i < MOGBUS_FAULT_KIND_COUNT; i++)
	{
		char	part[80];
		char	trigger[40];
		size_t	written;

		if (!info->faults[i].present)
			continue;
		if (info->faults[i].scheduled)
			snprintf(trigger, sizeof(trigger), "by schedule");
		else
			snprintf(trigger, sizeof(trigger), "%u%% of responses", info->faults[i].percent);
		switch ((mogbus_fault_kind_t)i)
		{
			case MOGBUS_FAULT_DROP       : snprintf(part, sizeof(part), "drop %s", trigger); break;
			case MOGBUS_FAULT_DELAY      : snprintf(part, sizeof(part), "delay %s by %u ms",
								trigger, info->faults[i].delay_ms); break;
			case MOGBUS_FAULT_EXCEPTION  : snprintf(part, sizeof(part), "exception %u on %s",
								info->faults[i].exception_code, trigger); break;
			case MOGBUS_FAULT_CORRUPT    : snprintf(part, sizeof(part), "corrupt %s", trigger); break;
			case MOGBUS_FAULT_DISCONNECT : snprintf(part, sizeof(part), "disconnect %s", trigger); break;
			case MOGBUS_FAULT_RESTART    : snprintf(part, sizeof(part), "restart %s", trigger); break;
			case MOGBUS_FAULT_SENSOR_DISCONNECT :
				snprintf(part, sizeof(part), "sensor-disconnect %s", trigger); break;
			default: continue;
		}
		written = (size_t)snprintf(buf + used, size - used, "%s%s",
					   used ? "; " : "", part);
		if (written >= size - used)
			break;
		used += written;
	}
	return used == 0 ? "none" : buf;
}

static void
print_rtu_timing(const mogbus_server_options_t* options)
{
	uint32_t	t35_us;
	uint32_t	gap_ms;

	mogbus_rtu_timing(options->baud, 0, &t35_us, &gap_ms);
	if (options->frame_gap_ms > 0)
		printf(", frame-gap %u ms (override)", options->frame_gap_ms);
	else if (options->baud > 0)
		printf(", baud %u, t3.5 %u us (frame-gap %u ms)", options->baud, t35_us, gap_ms);
	else
		printf(", frame-gap %u ms (default; baud unknown)", gap_ms);
}

static void
print_summary(mogbus_t* ctx, const mogbus_server_options_t* options)
{
	char		fault[512];
	const char*	serial_path = "pty";
	size_t		count       = mogbus_device_count(ctx);

	if (options->mode == MOGBUS_MODE_RTU_DEVICE ||
	    options->mode == MOGBUS_MODE_ASCII_DEVICE)
		serial_path = options->path;

	switch (options->mode)
	{
		case MOGBUS_MODE_RTU_TCP:
			printf("%zu device%s, RTU-over-TCP %s:%u",
			       count, count == 1 ? "" : "s", options->bind_address, options->port);
			print_rtu_timing(options);
			if (options->broadcast_execute)
				printf(", broadcast-execute on");
			putchar('\n');
			break;
		case MOGBUS_MODE_UDP:
			printf("%zu device%s, UDP %s:%u\n",
			       count, count == 1 ? "" : "s", options->bind_address, options->port);
			break;
		case MOGBUS_MODE_ASCII_UDP:
			printf("%zu device%s, ASCII-over-UDP %s:%u\n",
			       count, count == 1 ? "" : "s", options->bind_address, options->port);
			break;
		case MOGBUS_MODE_RTU_UDP:
			printf("%zu device%s, RTU-over-UDP %s:%u", count, count == 1 ? "" : "s",
			       options->bind_address, options->port);
			if (options->broadcast_execute)
				printf(", broadcast-execute on");
			putchar('\n');
			break;
		case MOGBUS_MODE_ASCII_TCP:
			printf("%zu device%s, ASCII-over-TCP %s:%u\n",
			       count, count == 1 ? "" : "s", options->bind_address, options->port);
			break;
		case MOGBUS_MODE_ASCII_PTY:
		case MOGBUS_MODE_ASCII_DEVICE:
			printf("%zu device%s, ASCII %s\n", count, count == 1 ? "" : "s", serial_path);
			break;
		case MOGBUS_MODE_RTU_PTY:
		case MOGBUS_MODE_RTU_DEVICE:
			printf("%zu device%s, RTU %s", count, count == 1 ? "" : "s", serial_path);
			print_rtu_timing(options);
			if (options->broadcast_execute)
				printf(", broadcast-execute on");
			putchar('\n');
			break;
		case MOGBUS_MODE_TCP:
			printf("%zu device%s on %s:%u", count, count == 1 ? "" : "s",
			       options->bind_address, options->port);
			if (options->tls_enabled)
				printf(" (TLS, client certificates %s)",
				       options->tls_require_client_certificate ? "required" : "optional");
			putchar('\n');
			break;
		case MOGBUS_MODE_RTU_FD:
		case MOGBUS_MODE_ASCII_FD:
			break;	/* host-only, never loaded from config */
	}

	if (options->http_bind_address)
		printf("  HTTP /healthz, /readyz and /metrics on %s:%u\n", options->http_bind_address,
		       options->http_port);
	for (size_t i = 0; i < count; i++)
	{
		mogbus_device_info_t info;

		mogbus_device_info(mogbus_device_at(ctx, i), &info);
		printf("  unit %-3u %-32s coils %u, discrete %u, input %u, holding %u, fault: %s\n",
		       info.unit_id, info.name, info.point_count[MOGBUS_COIL],
		       info.point_count[MOGBUS_DISCRETE], info.point_count[MOGBUS_INPUT],
		       info.point_count[MOGBUS_HOLDING], fault_text(&info, fault, sizeof(fault)));
		for (uint32_t f = 0; f < info.custom_fc_count; f++)
			printf("           function code 0x%02X%s%s\n", info.custom_fcs[f].code,
			       info.custom_fcs[f].name[0] ? " " : "", info.custom_fcs[f].name);
	}
}


int
main(int argc, char** argv)
{
	cli_t		cli     = { 0 };
	mogbus_t*	ctx     = NULL;
	mogbus_server_t* server = NULL;
	mogbus_server_options_t options;
	mogbus_error_t	failure = MOGBUS_OK;
	char		error[512];
	int		status  = 1;

	if (!parse_args(argc, argv, &cli))
		return 2;

	if (cli.show_version)
	{
#ifdef MOGBUS_GIT_REVISION
		printf("mogbus %s (%s)\n", mogbus_version(), MOGBUS_GIT_REVISION);
#else
		printf("mogbus %s\n", mogbus_version());
#endif
		return 0;
	}

	if (cli.show_help || !cli.config_path)
	{
		print_usage();
		return cli.show_help ? 0 : 2;
	}

	mogbus_set_log_level(cli.verbose ? MOGBUS_LOG_DEBUG : MOGBUS_LOG_INFO);
	mogbus_set_log_format(cli.log_json ? MOGBUS_LOG_JSON : MOGBUS_LOG_TEXT);
	ctx = mogbus_create();
	if (!ctx)
	{
		fprintf(stderr, "out of memory allocating app arena\n");
		return 1;
	}

	if (!mogbus_load_config(ctx, cli.config_path, error, sizeof(error)))
	{
		fprintf(stderr, "%s: %s\n", cli.config_path, error);
		goto cleanup;
	}

	mogbus_config_server_options(ctx, &options);
	if (cli.health)
	{
		mogbus_device_info_t first;

		if (mogbus_device_count(ctx) == 0 ||
		    !mogbus_device_info(mogbus_device_at(ctx, 0), &first))
		{
			fprintf(stderr, "%s: no device to probe\n", cli.config_path);
			goto cleanup;
		}
		if (!mogbus_probe(&options, first.unit_id, cli.timeout_ms ? cli.timeout_ms : 2000,
				  error, sizeof(error)))
		{
			fprintf(stderr, "unhealthy: %s\n", error);
			goto cleanup;
		}
		status = 0;
		goto cleanup;
	}
	print_summary(ctx, &options);
	if (cli.check_only)
	{
		if (!mogbus_server_check(ctx, &options, error, sizeof(error)))
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

	/* The transport logs why it failed; the exit status tells serial from listen/TLS errors. */
	server = mogbus_server_create_from_config(ctx, error, sizeof(error), &failure);
	if (!server)
	{
		if (failure == MOGBUS_ERR_INVALID)
			fprintf(stderr, "%s: %s\n", cli.config_path, error);
		status = failure == MOGBUS_ERR_SERIAL ? 1 : 2;
		goto cleanup;
	}
	g_server = server;
	if (mogbus_server_pty_name(server))
	{
		printf("%s PTY slave: %s\n", options.mode == MOGBUS_MODE_RTU_PTY ? "RTU" : "ASCII",
		       mogbus_server_pty_name(server));
		fflush(stdout);
	}
	status = mogbus_server_run(server);
	g_server = NULL;

cleanup:
	mogbus_server_close(server);
	mogbus_destroy(ctx);
	return status;
}
