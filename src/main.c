/**
 * @file
 * @brief mogbus: a Modbus device simulator configured with a Scheme file.
 *
 * A pure client of the public library API (mogbus.h); nothing here touches an internal module.
 */
#include "mogbus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cli_t cli_t;

/** @brief The context SIGINT/SIGTERM stop (see mogbus_stop_context_on_signals()). */
static mogbus_t* volatile	g_ctx;

/** @brief Parsed command line. */
struct cli_t
{
	const char*	config_path;
	const char*	server_id;	/* --server: the server --health probes and --web applies to */
	bool		check_only;
	bool		health;
	uint32_t	timeout_ms;
	bool		verbose;
	bool		show_help;
	bool		show_version;
	bool		log_json;
	bool		web;
	uint32_t	show_ms;	/* --show-registers interval, 0 = off */
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
	     "      --health         probe a server of the config (FC 08 echo to its first device)\n"
	     "                       and exit 0 if it answers, 1 if not; the first server unless\n"
	     "                       --server is given\n"
	     "      --server ID      the server --health probes and --web applies to (default: the\n"
	     "                       first one); every server of the config is always started\n"
	     "      --timeout-ms N   --health reply timeout (default 2000)\n"
	     "  -w, --web            serve the web UI (and the control API it uses) on the HTTP endpoint;\n"
	     "                       without (server (http ...)) in the config: 127.0.0.1 port 9102\n"
	     "  -r, --show-registers[=MS]\n"
	     "                       print the point values to stdout every MS ms (10-60000, default\n"
	     "                       1000): all of them first, then only the ones that changed;\n"
	     "                       JSON lines with --log-format json\n"
	     "  -v, --verbose        log every request\n"
	     "      --log-format F   log lines as text (default) or json\n"
	     "  -V, --version        print the version and exit\n"
	     "  -h, --help           show this help\n"
	     "\n"
	     "Transport/server settings (mode, path, port, bind, serial options) are\n"
	     "defined in (server ...) in the config file; there are no server CLI options.\n"
	     "A file with several (server (id \"NAME\") ...) runs them all together; each\n"
	     "device lists its servers with (servers \"NAME\" ...).\n"
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
		else if (strcmp(arg, "-w") == 0 || strcmp(arg, "--web") == 0)
			cli->web = true;
		else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--show-registers") == 0)
			cli->show_ms = 1000;
		else if (strncmp(arg, "--show-registers=", 17) == 0)
		{
			char* end = NULL;
			long  value = strtol(arg + 17, &end, 10);

			if (end == arg + 17 || *end != '\0' || value < 10 || value > 60000)
			{
				fprintf(stderr, "--show-registers needs a number of milliseconds (10-60000)\n");
				return false;
			}
			cli->show_ms = (uint32_t)value;
		}
		else if (strcmp(arg, "--health") == 0)
			cli->health = true;
		else if (strcmp(arg, "--server") == 0)
		{
			if (i + 1 >= argc)
			{
				fprintf(stderr, "--server needs a server id\n");
				return false;
			}
			cli->server_id = argv[++i];
		}
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

/** @brief True if the server with this id (NULL: a server without id) serves the device. */
static bool
serves(const mogbus_device_t* dev, const char* id)
{
	if (!id)
		return mogbus_device_server_at(dev, 0) == NULL;
	for (size_t i = 0; mogbus_device_server_at(dev, i); i++)
	{
		if (strcmp(mogbus_device_server_at(dev, i), id) == 0)
			return true;
	}
	return false;
}

static void
print_summary(mogbus_t* ctx, const mogbus_server_options_t* options)
{
	char		fault[512];
	const char*	serial_path = "pty";
	size_t		count       = 0;

	for (size_t i = 0; i < mogbus_device_count(ctx); i++)
		count += serves(mogbus_device_at(ctx, i), options->id);
	if (options->id)
		printf("[%s] ", options->id);

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
		printf("  HTTP /healthz, /readyz and /metrics on %s:%u%s\n", options->http_bind_address,
		       options->http_port, options->http_control || options->http_ui ? ", control on" : "");
	if (options->http_bind_address && options->http_ui)
	{
		const char* host = options->http_bind_address;

		/* A wildcard bind is reachable through loopback. */
		if (strcmp(host, "0.0.0.0") == 0)
			host = "127.0.0.1";
		else if (strcmp(host, "::") == 0)
			host = "::1";
		printf("  web UI on http://%s%s%s:%u/\n", strchr(host, ':') ? "[" : "", host,
		       strchr(host, ':') ? "]" : "", options->http_port);
	}
	for (size_t i = 0; i < mogbus_device_count(ctx); i++)
	{
		mogbus_device_info_t info;

		if (!serves(mogbus_device_at(ctx, i), options->id))
			continue;
		mogbus_device_info(mogbus_device_at(ctx, i), &info);
		printf("  unit %-3u %-32s coils %u, discrete %u, input %u, holding %u, fault: %s%s\n",
		       info.unit_id, info.name, info.point_count[MOGBUS_COIL],
		       info.point_count[MOGBUS_DISCRETE], info.point_count[MOGBUS_INPUT],
		       info.point_count[MOGBUS_HOLDING], fault_text(&info, fault, sizeof(fault)),
		       info.any_unit ? ", any unit" : "");
		for (uint32_t f = 0; f < info.custom_fc_count; f++)
			printf("           function code 0x%02X%s%s\n", info.custom_fcs[f].code,
			       info.custom_fcs[f].name[0] ? " " : "", info.custom_fcs[f].name);
	}
}

/** @brief Most points one device can have (DEVICE_MAX_POINTS in the library). */
enum { SHOW_MAX_POINTS = 8192 };

/** @brief FNV-1a hash of the last printed text of every point, to print only changes. */
static uint64_t g_shown[MOGBUS_MAX_DEVICES][SHOW_MAX_POINTS];

/**
 * @brief Prints the points whose value changed since the last call (all of them when @p all).
 *
 * One text line or JSON object per point, flushed at the end so a pipe sees whole snapshots.
 */
static void
show_registers(mogbus_t* ctx, bool all, bool json)
{
	static const char* const space_names[] = { "coil", "discrete", "input", "holding" };

	for (size_t d = 0; d < mogbus_device_count(ctx) && d < MOGBUS_MAX_DEVICES; d++)
	{
		mogbus_device_t*	dev = mogbus_device_at(ctx, d);
		mogbus_device_info_t	info;
		mogbus_point_t		point;

		if (!mogbus_device_info(dev, &info))
			continue;
		for (uint32_t i = 0; i < SHOW_MAX_POINTS && mogbus_device_point_at(dev, i, &point); i++)
		{
			char		text[2 * 123 + 1] = "";
			char		value[2 * 123 * 6 + 8];
			uint64_t	hash = 14695981039346656037ull;
			double		number = 0;
			bool		is_number = point.type != MOGBUS_TYPE_STRING &&
						    mogbus_device_get_value(dev, point.space, point.address, &number);

			if (point.type == MOGBUS_TYPE_STRING)
				(void)mogbus_device_get_string(dev, point.space, point.address, text, sizeof(text));
			if (is_number &&
			    !isfinite(number))
				snprintf(value, sizeof(value), json ? "null" : "%s",
					 isnan(number) ? "nan" : number < 0 ? "-inf" : "inf");
			else if (is_number)
				snprintf(value, sizeof(value),
					 point.type == MOGBUS_TYPE_F32 ? "%.7g" :
					 point.type == MOGBUS_TYPE_F64 ? "%.15g" : "%.0f", number);
			else
			{
				size_t used = 0;

				value[used++] = '"';
				for (const char* c = text; *c && used + 7 < sizeof(value); c++)
				{
					unsigned char ch = (unsigned char)*c;

					if (ch == '"' || ch == '\\')
					{
						value[used++] = '\\';
						value[used++] = (char)ch;
					}
					else if (ch < 0x20 || ch >= 0x7F)
						used += (size_t)snprintf(value + used, 7, "\\u%04x", ch);
					else
						value[used++] = (char)ch;
				}
				value[used++] = '"';
				value[used]   = '\0';
			}
			for (const char* c = value; *c; c++)
				hash = (hash ^ (unsigned char)*c) * 1099511628211ull;
			if (!all &&
			    hash == g_shown[d][i])
				continue;
			g_shown[d][i] = hash;
			if (json)
				printf("{\"unit\":%u,\"space\":\"%s\",\"address\":%u,\"value\":%s}\n",
				       info.unit_id, space_names[point.space], point.address, value);
			else
				printf("unit %u %s %u = %s\n", info.unit_id, space_names[point.space],
				       point.address, value);
		}
	}
	fflush(stdout);
}

int
main(int argc, char** argv)
{
	cli_t		cli     = { 0 };
	mogbus_t*	ctx     = NULL;
	mogbus_server_options_t options[MOGBUS_MAX_SERVERS];
	size_t		count   = 0;
	size_t		selected = 0;	/* the server --health probes and --web applies to */
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

	if (mogbus_load_config(ctx, cli.config_path) != MOGBUS_OK)
	{
		fprintf(stderr, "%s: %s\n", cli.config_path, mogbus_last_error(ctx));
		goto cleanup;
	}

	count = mogbus_config_server_count(ctx);
	for (size_t i = 0; i < count; i++)
		mogbus_config_server_options_at(ctx, i, &options[i]);
	if (cli.server_id)
	{
		for (selected = 0; selected < count; selected++)
		{
			if (options[selected].id &&
			    strcmp(options[selected].id, cli.server_id) == 0)
				break;
		}
		if (selected == count)
		{
			fprintf(stderr, "%s: no server with id '%s'\n", cli.config_path, cli.server_id);
			status = 2;
			goto cleanup;
		}
	}
	if (cli.web)
	{
		mogbus_server_options_t* web = &options[selected];

		if (!web->http_bind_address)
		{
			web->http_bind_address = "127.0.0.1";
			web->http_port         = mogbus_server_options_defaults().http_port;
		}
		web->http_ui      = true;
		web->http_control = true;
	}
	if (cli.health)
	{
		mogbus_device_t* first = NULL;

		for (size_t i = 0; i < mogbus_device_count(ctx) && !first; i++)
		{
			if (serves(mogbus_device_at(ctx, i), options[selected].id))
				first = mogbus_device_at(ctx, i);
		}
		mogbus_device_info_t info;

		if (!first ||
		    !mogbus_device_info(first, &info))
		{
			fprintf(stderr, "%s: no device to probe\n", cli.config_path);
			goto cleanup;
		}
		if (mogbus_probe(&options[selected], info.unit_id, cli.timeout_ms ? cli.timeout_ms : 2000) != MOGBUS_OK)
		{
			fprintf(stderr, "unhealthy: %s\n", mogbus_last_error(NULL));
			goto cleanup;
		}
		status = 0;
		goto cleanup;
	}
	for (size_t i = 0; i < count; i++)
		print_summary(ctx, &options[i]);
	if (cli.check_only)
	{
		for (size_t i = 0; i < count; i++)
		{
			if (mogbus_server_check(ctx, &options[i]) != MOGBUS_OK)
			{
				if (options[i].id)
					fprintf(stderr, "%s: server '%s': %s\n", cli.config_path, options[i].id,
						mogbus_last_error(ctx));
				else
					fprintf(stderr, "%s: cannot configure TLS: %s\n", cli.config_path,
						mogbus_last_error(ctx));
				goto cleanup;
			}
		}
		puts("config OK");
		status = 0;
		goto cleanup;
	}

	mogbus_stop_context_on_signals(&g_ctx);

	/* The transport logs why it failed; the exit status tells serial from listen/TLS errors. */
	for (size_t i = 0; i < count; i++)
	{
		mogbus_server_t* server = cli.web && i == selected ?
					  mogbus_server_create(ctx, &options[i]) :
					  mogbus_server_create_from_config_at(ctx, i);

		if (!server)
		{
			mogbus_error_t failure = mogbus_last_error_code(ctx);

			if (failure == MOGBUS_ERR_INVALID)
				fprintf(stderr, "%s: %s\n", cli.config_path, mogbus_last_error(ctx));
			status = failure == MOGBUS_ERR_SERIAL ? 1 : 2;
			goto cleanup;
		}
		if (mogbus_server_pty_name(server))
		{
			printf("%s%s%s%s PTY slave: %s\n", options[i].id ? "[" : "", options[i].id ? options[i].id : "",
			       options[i].id ? "] " : "",
			       options[i].mode == MOGBUS_MODE_RTU_PTY ? "RTU" : "ASCII",
			       mogbus_server_pty_name(server));
			fflush(stdout);
		}
	}
	g_ctx = ctx;
	if (cli.show_ms == 0)
		status = mogbus_run(ctx);
	else
	{
		int64_t	next = (int64_t)mogbus_now_ms();
		bool	all  = true;
		int	rc;

		do
		{
			int64_t	wait = next - (int64_t)mogbus_now_ms();

			if (wait <= 0)
			{
				show_registers(ctx, all, cli.log_json);
				all  = false;
				next = (int64_t)mogbus_now_ms() + cli.show_ms;
				wait = cli.show_ms;
			}
			rc = mogbus_step(ctx, (int)wait);
		}
		while (rc == MOGBUS_STEP_CONTINUE);
		status = rc == MOGBUS_STEP_ERROR ? 1 : 0;
	}
	g_ctx = NULL;

cleanup:
	mogbus_destroy(ctx);
	return status;
}
