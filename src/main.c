/* modbus-sim: a Modbus TCP device simulator configured with a Scheme file.
 * The vlib arena implementation lives in this file only. */
#include "device/fault.h"
#define ARENA_IMPL
#include "vlib/arena.h"
#include "vlib/sizes.h" // IWYU pragma: keep
#include "config/config.h"
#include "net/rtu_server.h"
#include "net/tcp_server.h"
#include "util/ip.h"
#include "util/log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#endif

typedef struct cli_t cli_t;
struct cli_t
{
	const char*	config_path;
	bool		check_only;
	bool		verbose;
	bool		show_help;
	bool		rtu_pty;
	s32		port_override;		/* -1 = use the config's port */
	s32		frame_gap_override;	/* -1 = use config/default */
	const char*	bind_override;		/* NULL = use the config's bind */
	const char*	rtu_device;		/* NULL = TCP mode */
};

static inline void
print_usage(void)
{
	puts("modbus-sim - Modbus TCP/RTU device simulator\n"
	     "\n"
	     "Usage: modbus-sim [options] CONFIG.scm\n"
	     "\n"
	     "Options:\n"
	     "  -p, --port N         TCP: listen on port N (overrides the config)\n"
	     "  -b, --bind ADDR      TCP: listen on IPv4 address ADDR (overrides the config)\n"
	     "      --rtu-pty        RTU: create a PTY and print its slave path\n"
	     "      --rtu-device PATH RTU: serve RTU over an existing serial-like device\n"
	     "      --frame-gap-ms N RTU: inter-frame silence in ms (overrides config, default 20)\n"
	     "  -c, --check          check the config, print a summary and exit\n"
	     "  -v, --verbose        log every request\n"
	     "  -h, --help           show this help\n"
	     "\n"
	     "RTU mode runs Modbus RTU over a bidirectional byte stream (PTY or device).\n"
	     "Frame boundaries use a fixed silence gap approximation (frame-gap-ms).\n"
	     "\n"
	     "Try:  modbus-sim profiles/minimal.scm\n"
	     "Docs: README.md and docs/");
}

/* Parses a positive integer argument strictly: digits only, no trailing junk, no overflow. */
static bool
parse_u32_arg(const char* text, u32 min, u32 max, const char* name, u32* out)
{
	char*		end  = NULL;
	unsigned long	value = 0;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno == ERANGE || end == text || *end != '\0' ||
	    text[0] < '0' || text[0] > '9' || value < min || value > max)
	{
		fprintf(stderr, "%s must be a whole number between %u and %u\n", name, min, max);
		return false;
	}

	*out = (u32)value;
	return true;
}

/* Returns false and prints a message when the arguments make no sense. */
static bool
parse_args(int argc, char** argv, cli_t* cli)
{
	cli->port_override      = -1;
	cli->frame_gap_override = -1;

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
		else if (strcmp(arg, "--rtu-pty") == 0)
			cli->rtu_pty = true;
		else if ((strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) && more)
		{
			u32 port = 0;

			if (!parse_u32_arg(argv[++i], 1, 65535, "port", &port))
				return false;
			cli->port_override = (s32)port;
		}
		else if ((strcmp(arg, "-b") == 0 || strcmp(arg, "--bind") == 0) && more)
			cli->bind_override = argv[++i];
		else if (strcmp(arg, "--rtu-device") == 0 && more)
			cli->rtu_device = argv[++i];
		else if (strcmp(arg, "--frame-gap-ms") == 0 && more)
		{
			u32 gap = 0;

			if (!parse_u32_arg(argv[++i], 1, 60000, "frame-gap-ms", &gap))
				return false;
			cli->frame_gap_override = (s32)gap;
		}
		else if (arg[0] != '-' && !cli->config_path)
			cli->config_path = arg;
		else
		{
			fprintf(stderr, "unexpected argument '%s' (try --help)\n", arg);
			return false;
		}
	}

	if (cli->rtu_pty && cli->rtu_device)
	{
		fprintf(stderr, "--rtu-pty and --rtu-device cannot be used together\n");
		return false;
	}
	if (cli->port_override != -1 && (cli->rtu_pty || cli->rtu_device))
	{
		fprintf(stderr, "--port is only valid in TCP mode\n");
		return false;
	}
	if (cli->bind_override && (cli->rtu_pty || cli->rtu_device))
	{
		fprintf(stderr, "--bind is only valid in TCP mode\n");
		return false;
	}
	if (cli->frame_gap_override != -1 && !cli->rtu_pty && !cli->rtu_device)
	{
		fprintf(stderr, "--frame-gap-ms is only valid in RTU mode\n");
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

static bool
validate_rtu_unit_ids(const config_t* config, char* err, u32 err_size)
{
	for (u32 i = 0; i < config->device_count; i++)
	{
		u8 id = config->devices[i]->unit_id;

		if (id == 0 || id > 247)
		{
			snprintf(err, err_size,
				 "device '%s': unit-id %u is not valid for RTU (allowed: 1-247; 0 is broadcast, 248-255 reserved)",
				 config->devices[i]->name, id);
			return false;
		}
	}
	return true;
}

static void
warn_rtu_unit_ids(const config_t* config)
{
	for (u32 i = 0; i < config->device_count; i++)
	{
		u8 id = config->devices[i]->unit_id;
		if (id == 0 || id > 247)
			fprintf(stderr, "warning: device '%s': unit-id %u is not valid for RTU (allowed: 1-247; 0 is broadcast, 248-255 reserved)\n",
				config->devices[i]->name, id);
	}
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
print_summary(const config_t* config, bool rtu_mode, const char* rtu_path)
{
	char	fault[96];

	if (rtu_mode)
		printf("%u device%s, RTU %s, frame-gap %u ms\n",
		       config->device_count, config->device_count == 1 ? "" : "s",
		       rtu_path ? rtu_path : "(fd)", config->frame_gap_ms);
	else
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

	if (cli.frame_gap_override != -1)
		config.frame_gap_ms = (u32)cli.frame_gap_override;

	{
		bool rtu_mode = cli.rtu_pty || cli.rtu_device;
		if (rtu_mode)
		{
			if (!validate_rtu_unit_ids(&config, error, sizeof(error)))
			{
				fprintf(stderr, "%s: %s\n", cli.config_path, error);
				arena_release(app);
				return 1;
			}
		}
		else if (cli.check_only)
			warn_rtu_unit_ids(&config);
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

	{
		bool		rtu_mode  = cli.rtu_pty || cli.rtu_device;
		const char*	rtu_path  = cli.rtu_device ? cli.rtu_device : (cli.rtu_pty ? "pty" : NULL);

		print_summary(&config, rtu_mode, rtu_path);
		if (cli.check_only)
		{
			puts("config OK");
			arena_release(app);
			return 0;
		}

		if (rtu_mode)
		{
			int	fd       = -1;
			int	slave_fd = -1;
			s32	status;

			if (cli.rtu_pty)
			{
#if defined(__linux__)
				char		name[128];
				struct termios	tio;

				if (openpty(&fd, &slave_fd, name, NULL, NULL) < 0)
				{
					fprintf(stderr, "cannot create PTY: %s\n", strerror(errno));
					arena_release(app);
					return 1;
				}
				if (tcgetattr(fd, &tio) == 0)
				{
					cfmakeraw(&tio);
					(void)tcsetattr(fd, TCSANOW, &tio);
				}
				if (tcgetattr(slave_fd, &tio) == 0)
				{
					cfmakeraw(&tio);
					(void)tcsetattr(slave_fd, TCSANOW, &tio);
				}
				printf("RTU PTY slave: %s\n", name);
				fflush(stdout);
#else
				fprintf(stderr, "--rtu-pty is not supported on this platform\n");
				arena_release(app);
				return 2;
#endif
			}
			else
			{
				fd = open(cli.rtu_device, O_RDWR | O_NOCTTY);
				if (fd < 0)
				{
					fprintf(stderr, "cannot open RTU device '%s': %s\n",
						cli.rtu_device, strerror(errno));
					arena_release(app);
					return 1;
				}
			}

			if (isatty(fd))
			{
				struct termios tio;

				if (tcgetattr(fd, &tio) == 0)
				{
					cfmakeraw(&tio);
					(void)tcsetattr(fd, TCSANOW, &tio);
				}
			}

			{
				int flags = fcntl(fd, F_GETFL, 0);
				if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
				{
					fprintf(stderr, "cannot set RTU fd non-blocking: %s\n", strerror(errno));
					if (slave_fd >= 0)
						close(slave_fd);
					close(fd);
					arena_release(app);
					return 1;
				}
			}

			rtu_server_options_t rtu_options = {
				.fd           = fd,
				.seed         = config.seed,
				.frame_gap_ms = config.frame_gap_ms,
				.devices      = config.devices,
				.device_count = config.device_count,
			};
			status = rtu_server_run(&rtu_options);
			if (slave_fd >= 0)
				close(slave_fd);
			close(fd);
			arena_release(app);
			return status;
		}
	}

	{
		tcp_server_options_t options = {
			.bind_address    = config.bind_address,
			.port            = config.port,
			.seed            = config.seed,
			.idle_timeout_ms = config.idle_timeout_ms,
			.devices         = config.devices,
			.device_count    = config.device_count,
		};
		s32 status = tcp_server_run(&options);

		arena_release(app);
		return status;
	}
}
