/**
 * @file
 * @brief mogbus: a Modbus device simulator configured with a Scheme file.
 *
 * A pure client of the public library API (mogbus.h); nothing here touches an internal module.
 */
#include "mogbus.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME_ARGS	256

typedef enum check_t check_t;
typedef struct cli_t cli_t;

/** @brief The context SIGINT/SIGTERM stop (see mogbus_stop_context_on_signals()). */
static mogbus_t* volatile	g_ctx;

/** @brief What --check does. */
enum check_t
{
	CHECK_OFF,
	CHECK_QUIET,	/**< validate, print only the error */
	CHECK_VERBOSE,	/**< also print the summary and "config OK" */
};

/** @brief Parsed command line. */
struct cli_t
{
	const char*	config_path;
	const char*	server_id;	/* --server: the server --health probes and --web applies to */
	check_t		check;
	bool		health;
	uint32_t	timeout_ms;
	bool		verbose;
	bool		show_help;
	bool		show_version;
	bool		log_json;
	bool		web;
	uint32_t	show_ms;	/* --show-registers interval, 0 = off */
	bool		decode;		/* --decode: the positional arguments are frames */
	bool		framing_set;
	bool		direction_set;
	mogbus_framing_t	framing;
	mogbus_direction_t	direction;
	size_t		frame_count;
	const char*	frames[MAX_FRAME_ARGS];
};

static inline void
print_usage(void)
{
	puts("mogbus - Modbus TCP/UDP/RTU/ASCII device simulator\n"
	     "\n"
	     "Usage: mogbus [options] CONFIG.scm\n"
	     "       mogbus --decode [--framing F] [--direction D] [HEX...]\n"
	     "\n"
	     "Options:\n"
	     "  -c, --check[=verbose]\n"
	     "                       check the config and exit: prints nothing when it is valid,\n"
	     "                       the error when not; =verbose prints the summary and config OK\n"
	     "      --health         probe a server of the config (FC 08 echo to its first device)\n"
	     "                       and exit 0 if it answers, 1 if not; the first server unless\n"
	     "                       --server is given\n"
	     "      --server ID      the server --health probes and --web applies to (default: the\n"
	     "                       first one); every server of the config is always started\n"
	     "      --timeout-ms N   --health reply timeout (default 2000)\n"
	     "  -w, --web            serve the web UI (and the control API it uses) on the HTTP endpoint;\n"
	     "                       without (server (http ...)) in the config: 127.0.0.1 port 9102\n"
	     "  -r, --show-registers[=MS]\n"
	     "                       show the point values every MS ms (10-60000, default 1000): one\n"
	     "                       table redrawn in place on a terminal (logs go below it), else\n"
	     "                       all points as text lines, then only the changes; JSON lines\n"
	     "                       with --log-format json\n"
	     "      --decode         decode captured frames (hex, or ASCII text starting with ':')\n"
	     "                       given as arguments or, with none, one per line on stdin; prints\n"
	     "                       the ADU and PDU fields (--log-format json: one object per frame);\n"
	     "                       exit 1 if a frame is malformed. No config is read\n"
	     "      --framing F      --decode framing: auto (default), mbap, rtu or ascii\n"
	     "      --direction D    --decode direction: auto (default), request or response\n"
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

/** @brief Parses a decimal number in [min, max]; false for anything else. */
static bool
parse_number(const char* text, long min, long max, uint32_t* out)
{
	char*	end   = "";
	long	value;

	if (!text ||
	    !*text)
		return false;
	value = strtol(text, &end, 10);
	if (value < min ||
	    value > max ||
	    *end != '\0')
		return false;
	*out = (uint32_t)value;
	return true;
}

/** @brief Returns false and prints a message when the arguments make no sense. */
static bool
parse_args(int argc, char** argv, cli_t* cli)
{
	static const struct flag_t
	{
		const char*	short_name;
		const char*	name;
		size_t		offset;
	} flags[] = {
		{ "-h", "--help",    offsetof(cli_t, show_help)    },
		{ "-V", "--version", offsetof(cli_t, show_version) },
		{ "-w", "--web",     offsetof(cli_t, web)          },
		{ "-v", "--verbose", offsetof(cli_t, verbose)      },
		{ NULL, "--health",  offsetof(cli_t, health)       },
	};

	for (int i = 1; i < argc; i++)
	{
		const char*	arg   = argv[i];
		const char*	value = i + 1 < argc ? argv[i + 1] : NULL;
		bool		known = false;

		for (size_t f = 0; f < sizeof(flags) / sizeof(flags[0]); f++)
		{
			if (strcmp(arg, flags[f].name) == 0 ||
			    (flags[f].short_name && strcmp(arg, flags[f].short_name) == 0))
			{
				*(bool*)((char*)cli + flags[f].offset) = true;
				known = true;
			}
		}
		if (known)
			continue;

		if (strcmp(arg, "-c") == 0 ||
		    strcmp(arg, "--check") == 0)
			cli->check = CHECK_QUIET;
		else if (strcmp(arg, "--check=verbose") == 0)
			cli->check = CHECK_VERBOSE;
		else if (strncmp(arg, "--check=", 8) == 0)
		{
			fprintf(stderr, "--check takes no value or =verbose\n");
			return false;
		}
		else if (strcmp(arg, "-r") == 0 ||
			 strcmp(arg, "--show-registers") == 0)
			cli->show_ms = 1000;
		else if (strncmp(arg, "--show-registers=", 17) == 0)
		{
			if (!parse_number(arg + 17, 10, 60000, &cli->show_ms))
			{
				fprintf(stderr, "--show-registers needs a number of milliseconds (10-60000)\n");
				return false;
			}
		}
		else if (strcmp(arg, "--server") == 0)
		{
			if (!value)
			{
				fprintf(stderr, "--server needs a server id\n");
				return false;
			}
			cli->server_id = argv[++i];
		}
		else if (strcmp(arg, "--timeout-ms") == 0)
		{
			if (!parse_number(value, 1, 600000, &cli->timeout_ms))
			{
				fprintf(stderr, "--timeout-ms needs a number of milliseconds (1-600000)\n");
				return false;
			}
			i++;
		}
		else if (strcmp(arg, "--log-format") == 0)
		{
			if (!value ||
			    (strcmp(value, "json") != 0 && strcmp(value, "text") != 0))
			{
				fprintf(stderr, "--log-format needs text or json\n");
				return false;
			}
			cli->log_json = strcmp(value, "json") == 0;
			i++;
		}
		else if (strcmp(arg, "--decode") == 0)
			cli->decode = true;
		else if (strcmp(arg, "--framing") == 0)
		{
			if (!value ||
			    !(strcmp(value, "auto") == 0 || strcmp(value, "mbap") == 0 ||
			      strcmp(value, "rtu") == 0 || strcmp(value, "ascii") == 0))
			{
				fprintf(stderr, "--framing needs auto, mbap, rtu or ascii\n");
				return false;
			}
			cli->framing     = strcmp(value, "mbap") == 0 ? MOGBUS_FRAMING_MBAP :
					   strcmp(value, "rtu") == 0 ? MOGBUS_FRAMING_RTU :
					   strcmp(value, "ascii") == 0 ? MOGBUS_FRAMING_ASCII : MOGBUS_FRAMING_AUTO;
			cli->framing_set = true;
			i++;
		}
		else if (strcmp(arg, "--direction") == 0)
		{
			if (!value ||
			    !(strcmp(value, "auto") == 0 || strcmp(value, "request") == 0 ||
			      strcmp(value, "response") == 0))
			{
				fprintf(stderr, "--direction needs auto, request or response\n");
				return false;
			}
			cli->direction     = strcmp(value, "request") == 0 ? MOGBUS_DIRECTION_REQUEST :
					     strcmp(value, "response") == 0 ? MOGBUS_DIRECTION_RESPONSE :
					     MOGBUS_DIRECTION_AUTO;
			cli->direction_set = true;
			i++;
		}
		else if (arg[0] == '-')
		{
			fprintf(stderr, "unknown option '%s' (try --help)\n", arg);
			return false;
		}
		else if (cli->frame_count < MAX_FRAME_ARGS)
			cli->frames[cli->frame_count++] = arg;	/* the config, or the frames with --decode */
		else
		{
			fprintf(stderr, "too many arguments (try --help)\n");
			return false;
		}
	}
	if (!cli->decode &&
	    (cli->framing_set || cli->direction_set))
	{
		fprintf(stderr, "--framing and --direction need --decode\n");
		return false;
	}
	if (!cli->decode &&
	    cli->frame_count > 0)
	{
		cli->config_path = cli->frames[0];
		if (cli->frame_count > 1)
		{
			fprintf(stderr, "unexpected argument '%s' (try --help)\n", cli->frames[1]);
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

static const char* const space_names[] = { "coil", "discrete", "input", "holding" };

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

/** @brief Prints `prefix` and http://HOST:PORT/ (IPv6 hosts in brackets), no newline. */
static void
print_url(const char* prefix, const char* host, uint16_t port)
{
	bool ipv6 = strchr(host, ':') != NULL;

	printf("%shttp://%s%s%s:%u/", prefix, ipv6 ? "[" : "", host, ipv6 ? "]" : "", port);
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
	if (options->http_bind_address &&
	    options->http_ui)
	{
		const char* host     = options->http_bind_address;
		bool        wildcard = strcmp(host, "0.0.0.0") == 0 ||
				       strcmp(host, "::") == 0;

		print_url("  web UI on ", host, options->http_port);
		if (wildcard)
			print_url(" (reachable at ", host[0] == ':' ? "::1" : "127.0.0.1", options->http_port);
		puts(wildcard ? ")" : "");
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
		for (uint32_t a = 0;; a++)
		{
			mogbus_action_t action;

			if (!mogbus_device_action_at(mogbus_device_at(ctx, i), a, &action))
				break;
			printf("           %s %u %s%s%s", space_names[action.space], action.address,
			       action.on_write ? "on-write" : "on-read",
			       action.name[0] ? " " : "", action.name);
			for (uint32_t p = 0; p < action.param_count; p++)
			{
				if (action.params[p].is_bool)
					printf(" (%s %s)", action.params[p].name,
					       action.params[p].flag ? "#t" : "#f");
				else
					printf(" (%s %g)", action.params[p].name,
					       action.params[p].number);
			}
			putchar('\n');
		}
	}
}

/** @brief Index of the server --server names (0 without it); false when there is none. */
static bool
select_server(const cli_t* cli, const mogbus_server_options_t* options, size_t count, size_t* selected)
{
	*selected = 0;
	if (!cli->server_id)
		return true;
	for (size_t i = 0; i < count; i++)
	{
		if (options[i].id &&
		    strcmp(options[i].id, cli->server_id) == 0)
		{
			*selected = i;
			return true;
		}
	}
	return false;
}

/** @brief --health: probes the first device of the selected server; the exit status. */
static int
run_health(const cli_t* cli, mogbus_t* ctx, const mogbus_server_options_t* options)
{
	mogbus_device_info_t	info;
	mogbus_device_t*	first = NULL;

	for (size_t i = 0; i < mogbus_device_count(ctx) && !first; i++)
	{
		if (serves(mogbus_device_at(ctx, i), options->id))
			first = mogbus_device_at(ctx, i);
	}
	if (!first ||
	    !mogbus_device_info(first, &info))
	{
		fprintf(stderr, "%s: no device to probe\n", cli->config_path);
		return 1;
	}
	if (mogbus_probe(options, info.unit_id, cli->timeout_ms ? cli->timeout_ms : 2000) != MOGBUS_OK)
	{
		fprintf(stderr, "unhealthy: %s\n", mogbus_last_error(NULL));
		return 1;
	}
	return 0;
}

/** @brief --check: validates every server (TLS files, ciphers); the exit status. */
static int
run_check(const cli_t* cli, mogbus_t* ctx, const mogbus_server_options_t* options, size_t count)
{
	for (size_t i = 0; i < count; i++)
	{
		if (mogbus_server_check(ctx, &options[i]) == MOGBUS_OK)
			continue;
		if (options[i].id)
			fprintf(stderr, "%s: server '%s': %s\n", cli->config_path, options[i].id, mogbus_last_error(ctx));
		else
			fprintf(stderr, "%s: cannot configure TLS: %s\n", cli->config_path, mogbus_last_error(ctx));
		return 1;
	}
	if (cli->check == CHECK_VERBOSE)
		puts("config OK");
	return 0;
}

/** @brief Starts every server and runs them until stopped; the exit status. */
static int
run_servers(const cli_t* cli, mogbus_t* ctx, const mogbus_server_options_t* options, size_t count)
{
	int status;

	mogbus_stop_context_on_signals(&g_ctx);

	/* The transport logs why it failed; the exit status tells serial from listen/TLS errors. */
	for (size_t i = 0; i < count; i++)
	{
		mogbus_server_t*	server  = NULL;
		mogbus_result_t		failure = mogbus_server_create(ctx, &options[i], &server);

		if (failure != MOGBUS_OK)
		{
			if (failure == MOGBUS_ERR_INVALID)
				fprintf(stderr, "%s: %s\n", cli->config_path, mogbus_last_error(ctx));
			return failure == MOGBUS_ERR_SERIAL ? 1 : 2;
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
	g_ctx  = ctx;
	status = mogbus_run(ctx);
	g_ctx  = NULL;
	return status;
}

/** @brief Everything after the arguments are known; the exit status. */
static int
run(const cli_t* cli, mogbus_t* ctx)
{
	mogbus_server_options_t	options[MOGBUS_MAX_SERVERS];
	size_t			count;
	size_t			selected;

	if (mogbus_load_config(ctx, cli->config_path) != MOGBUS_OK)
	{
		fprintf(stderr, "%s: %s\n", cli->config_path, mogbus_last_error(ctx));
		return 1;
	}
	count = mogbus_config_server_count(ctx);
	for (size_t i = 0; i < count; i++)
	{
		if (mogbus_config_server_options_at(ctx, i, &options[i]) != MOGBUS_OK)
			count = 0;
	}
	if (count == 0)
	{
		fprintf(stderr, "%s: no server section could be read\n", cli->config_path);
		return 1;
	}
	if (!select_server(cli, options, count, &selected))
	{
		fprintf(stderr, "%s: no server with id '%s'\n", cli->config_path, cli->server_id);
		return 2;
	}
	if (cli->web)
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
	if (cli->show_ms)
	{
		for (size_t i = 0; i < count; i++)
			options[i].show_registers_ms = cli->show_ms;
	}
	if (cli->health)
		return run_health(cli, ctx, &options[selected]);
	if (cli->check != CHECK_QUIET)
	{
		for (size_t i = 0; i < count; i++)
			print_summary(ctx, &options[i]);
	}
	if (cli->check != CHECK_OFF)
		return run_check(cli, ctx, options, count);
	return run_servers(cli, ctx, options, count);
}

/** @brief A hex digit's value, or -1. */
static int
hex_value(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

/**
 * @brief Turns a frame argument into bytes: hex (spaces, ':', ',', '-' and 0x prefixes between
 *        bytes are ignored), or, when it starts with ':', the ASCII frame text itself (CR LF added
 *        when missing).
 */
static bool
parse_frame_text(const char* text, uint8_t* out, size_t cap, size_t* len)
{
	size_t	n = 0;
	int	high = -1;

	while (*text == ' ' ||
	       *text == '\t')
		text++;
	if (*text == ':')
	{
		size_t end = strlen(text);

		while (end > 0 &&
		       (text[end - 1] == '\r' || text[end - 1] == '\n' || text[end - 1] == ' ' ||
			text[end - 1] == '\t'))
			end--;
		if (end >= 4 &&
		    memcmp(text + end - 4, "\\r\\n", 4) == 0)
			end -= 4;
		if (end + 2 > cap)
			return false;
		memcpy(out, text, end);
		out[end]     = '\r';
		out[end + 1] = '\n';
		*len         = end + 2;
		return true;
	}
	for (; *text; text++)
	{
		int v;

		if (*text == ' ' ||
		    *text == '\t' ||
		    *text == '\r' ||
		    *text == '\n' ||
		    *text == ':' ||
		    *text == ',' ||
		    *text == '-')
			continue;
		if (*text == '0' &&
		    high < 0 &&
		    (text[1] == 'x' || text[1] == 'X'))
		{
			text++;
			continue;
		}
		v = hex_value(*text);
		if (v < 0)
		{
			fprintf(stderr, "invalid character '%c' in frame\n", *text);
			return false;
		}
		if (high < 0)
			high = v;
		else
		{
			if (n >= cap)
				return false;
			out[n++] = (uint8_t)((high << 4) | v);
			high     = -1;
		}
	}
	if (high >= 0)
	{
		fprintf(stderr, "odd number of hex digits in frame\n");
		return false;
	}
	*len = n;
	return true;
}

/** @brief Decodes one frame argument or stdin line; the exit status it contributes. */
static int
decode_one(const cli_t* cli, const char* text, bool first)
{
	static uint8_t	frame[4096];
	static char	out[MOGBUS_DECODE_BUFFER_SIZE];
	size_t		frame_len = 0;
	size_t		out_len   = 0;
	mogbus_result_t	result;

	if (!parse_frame_text(text, frame, sizeof(frame), &frame_len))
	{
		fprintf(stderr, "cannot read the frame '%.40s'\n", text);
		return 2;
	}
	result = mogbus_decode_frame(frame, frame_len, cli->framing, cli->direction, cli->log_json, out,
				     sizeof(out), &out_len);
	if (result != MOGBUS_OK &&
	    result != MOGBUS_ERR_FRAME)
	{
		fprintf(stderr, "cannot decode the frame '%.40s'\n", text);
		return 2;
	}
	if (!cli->log_json &&
	    !first)
		putchar('\n');
	fputs(out, stdout);
	if (cli->log_json)
		putchar('\n');
	return result == MOGBUS_ERR_FRAME ? 1 : 0;
}

/** @brief --decode: 0 if every frame decoded, 1 if one is malformed, 2 for unreadable input. */
static int
run_decode(const cli_t* cli)
{
	static char	line[16384];
	int		status = 0;
	bool		first  = true;

	for (size_t i = 0; i < cli->frame_count; i++)
	{
		int one = decode_one(cli, cli->frames[i], first);

		status = one > status ? one : status;
		first  = false;
	}
	if (cli->frame_count > 0)
		return status;
	while (fgets(line, sizeof(line), stdin))
	{
		const char*	p = line;
		int		one;

		while (*p == ' ' ||
		       *p == '\t')
			p++;
		if (*p == '\0' ||
		    *p == '\n' ||
		    *p == '\r' ||
		    *p == '#')
			continue;
		one    = decode_one(cli, line, first);
		status = one > status ? one : status;
		first  = false;
	}
	return status;
}

int
main(int argc, char** argv)
{
	cli_t		cli = { 0 };
	mogbus_t*	ctx = NULL;
	int		status;

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

	if (cli.decode &&
	    !cli.show_help)
		return run_decode(&cli);

	if (cli.show_help ||
	    !cli.config_path)
	{
		print_usage();
		return cli.show_help ? 0 : 2;
	}

	mogbus_set_log_level(cli.verbose ? MOGBUS_LOG_DEBUG : MOGBUS_LOG_INFO);
	mogbus_set_log_format(cli.log_json ? MOGBUS_LOG_JSON : MOGBUS_LOG_TEXT);
	if (mogbus_create(&ctx) != MOGBUS_OK)
	{
		fprintf(stderr, "%s\n", mogbus_last_error(NULL));
		return 1;
	}
	status = run(&cli, ctx);
	mogbus_destroy(ctx);
	return status;
}
