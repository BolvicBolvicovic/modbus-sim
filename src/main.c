/* modbus-sim: a Modbus TCP device simulator configured with a Scheme file.
 * The vlib arena implementation lives in this file only. */
#include "device/fault.h"
#define ARENA_IMPL
#include "vlib/arena.h"
#include "vlib/sizes.h" // IWYU pragma: keep
#include "config/config.h"
#include "net/server.h"
#include "util/ip.h"
#include "util/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cli_t cli_t;
struct cli_t
{
	const char*	config_path;
	bool		check_only;
	bool		verbose;
	bool		show_help;
	s32		port_override;		/* -1 = use the config's port */
	const char*	bind_override;		/* NULL = use the config's bind */
};

static inline void
print_usage(void)
{
	puts("modbus-sim - Modbus TCP device simulator\n"
	     "\n"
	     "Usage: modbus-sim [options] CONFIG.scm\n"
	     "\n"
	     "Options:\n"
	     "  -p, --port N     listen on port N (overrides the config)\n"
	     "  -b, --bind ADDR  listen on IPv4 address ADDR (overrides the config)\n"
	     "  -c, --check      check the config, print a summary and exit\n"
	     "  -v, --verbose    log every request\n"
	     "  -h, --help       show this help\n"
	     "\n"
	     "Try:  modbus-sim profiles/minimal.scm\n"
	     "Docs: README.md and docs/");
}

/* Parses a port argument strictly: digits only, no trailing junk, no overflow. */
static bool
parse_port(const char* text, s32* out)
{
	char*	end  = NULL;
	long	value = 0;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno == ERANGE || end == text || *end != '\0' ||
	    text[0] < '0' || text[0] > '9' || value < 1 || value > 65535)
	{
		fprintf(stderr, "port must be a whole number between 1 and 65535\n");
		return false;
	}

	*out = (s32)value;
	return true;
}

/* Returns false and prints a message when the arguments make no sense. */
static bool
parse_args(int argc, char** argv, cli_t* cli)
{
	cli->port_override = -1;

	for (int i = 1; i < argc; i++)
	{
		const char*	arg  = argv[i];
		bool		more = (i + 1 < argc);

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
			cli->show_help = true;
		else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--check") == 0)
			cli->check_only = true;
		else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)
			cli->verbose = true;
		else if ((strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) && more)
		{
			if (!parse_port(argv[++i], &cli->port_override))
				return false;
		}
		else if ((strcmp(arg, "-b") == 0 || strcmp(arg, "--bind") == 0) && more)
			cli->bind_override = argv[++i];
		else if (arg[0] != '-' && !cli->config_path)
			cli->config_path = arg;
		else
		{
			fprintf(stderr, "unexpected argument '%s' (try --help)\n", arg);
			return false;
		}
	}

	if (cli->port_override != -1 && (cli->port_override < 1 || cli->port_override > 65535))
	{
		fprintf(stderr, "port must be between 1 and 65535\n");
		return false;
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
		case FAULT_NONE : return "none";
		case FAULT_DROP : snprintf(buf, size, "drop %u%% of responses", fault->percent); break;
		case FAULT_DELAY: snprintf(buf, size, "delay %u%% of responses by %u ms", fault->percent, fault->delay_ms); break;
	}

	return buf;
}

static void
print_summary(const config_t* config)
{
	char	fault[96];

	printf("%u device%s on %s:%u\n", config->device_count, config->device_count == 1 ? "" : "s",
	       config->bind_address, config->port);

	for (u32 i = 0; i < config->device_count; i++)
	{
		const device_t*	dev = config->devices[i];

		printf("  unit %-3u %-32s coils %u, discrete %u, input %u, holding %u, fault: %s\n",
		       dev->unit_id, dev->name, count_points(dev, SPACE_COIL),
		       count_points(dev, SPACE_DISCRETE), count_points(dev, SPACE_INPUT),
		       count_points(dev, SPACE_HOLDING), fault_text(&dev->fault, fault, sizeof(fault)));
	}
}

int
main(int argc, char** argv)
{
	cli_t		cli      = { 0 };
	config_t	config   = { 0 };
	char		error[512];
	arena_t*	app      = NULL;
	arena_t*	scratch  = NULL;
	bool		loaded   = false;

	if (!parse_args(argc, argv, &cli))
		return 2;

	if (cli.show_help || !cli.config_path)
	{
		print_usage();
		return cli.show_help ? 0 : 2;
	}

	log_set_verbose(cli.verbose);
	app     = ARENA_ALLOC();
	scratch = ARENA_ALLOC();

	loaded = config_load(app, scratch, cli.config_path, &config, error, sizeof(error));
	arena_release(scratch);
	if (!loaded)
	{
		fprintf(stderr, "%s: %s\n", cli.config_path, error);
		arena_release(app);
		return 1;
	}

	if (cli.port_override != -1)
		config.port = (u16)cli.port_override;

	if (cli.bind_override)
	{
		if (strlen(cli.bind_override) >= sizeof(config.bind_address))
		{
			fprintf(stderr, "bind address is too long (max %zu characters)\n",
				sizeof(config.bind_address) - 1);
			arena_release(app);
			return 2;
		}
		if (!ip_is_ipv4_literal(cli.bind_override))
		{
			fprintf(stderr, "bind address '%s' is not an IPv4 address like 127.0.0.1\n",
				cli.bind_override);
			arena_release(app);
			return 2;
		}
		strcpy(config.bind_address, cli.bind_override);
	}

	print_summary(&config);
	if (cli.check_only)
	{
		puts("config OK");
		arena_release(app);
		return 0;
	}

	server_options_t	options = {
		.bind_address = config.bind_address,
		.port         = config.port,
		.seed         = config.seed,
		.devices      = config.devices,
		.device_count = config.device_count,
	};
	s32			status = server_run(&options);

	arena_release(app);
	return status;
}
