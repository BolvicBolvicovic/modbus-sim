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
	s32		frame_gap_override;	/* -1 = use config/auto */
	s32		baud_override;		/* -1 = use config/tty/default */
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
	     "      --baud N         RTU: serial baud rate for t3.5 timing (overrides config/tty)\n"
	     "      --frame-gap-ms N RTU: explicit inter-frame silence override in ms\n"
	     "  -c, --check          check the config, print a summary and exit\n"
	     "  -v, --verbose        log every request\n"
	     "  -h, --help           show this help\n"
	     "\n"
	     "Exit status: 0 success, 1 runtime/config-load error, 2 invalid CLI or config option.\n"
	     "\n"
	     "RTU mode runs Modbus RTU over a bidirectional byte stream (PTY or device).\n"
	     "Frame boundaries use Modbus t3.5 when baud is known, or frame-gap-ms/default.\n"
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
	cli->baud_override      = -1;

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
		else if ((strcmp(arg, "--baud") == 0 || strcmp(arg, "--rtu-baud") == 0) && more)
		{
			u32 baud = 0;

			if (!parse_u32_arg(argv[++i], 1, 4000000, "baud", &baud))
				return false;
			cli->baud_override = (s32)baud;
		}
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
	if (cli->baud_override != -1 && !cli->rtu_pty && !cli->rtu_device)
	{
		fprintf(stderr, "--baud is only valid in RTU mode\n");
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
	{
		printf("%u device%s, RTU %s",
		       config->device_count, config->device_count == 1 ? "" : "s",
		       rtu_path ? rtu_path : "(fd)");
		if (config->frame_gap_ms > 0)
			printf(", frame-gap %u ms (override)\n", config->frame_gap_ms);
		else if (config->baud > 0)
			printf(", baud %u, t3.5 %u us (frame-gap %u ms)\n",
			       config->baud, rtu_t35_us(config->baud),
			       rtu_effective_gap_ms(0, config->baud));
		else
			printf(", frame-gap %u ms (default; baud unknown)\n",
			       rtu_effective_gap_ms(0, 0));
	}
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

static s32
apply_overrides(const cli_t* cli, config_t* config, const char* config_path)
{
	bool	rtu_mode = cli->rtu_pty || cli->rtu_device;
	char	error[512];

	if (cli->frame_gap_override != -1)
		config->frame_gap_ms = (u32)cli->frame_gap_override;
	if (cli->baud_override != -1)
		config->baud = (u32)cli->baud_override;

	if (rtu_mode && !validate_rtu_unit_ids(config, error, sizeof(error)))
	{
		fprintf(stderr, "%s: %s\n", config_path, error);
		return 2;
	}

	if (cli->port_override != -1)
		config->port = (u16)cli->port_override;

	if (!cli->bind_override)
		return 0;

	if (strlen(cli->bind_override) >= sizeof(config->bind_address))
	{
		fprintf(stderr, "bind address is too long (max %zu characters)\n",
			sizeof(config->bind_address) - 1);
		return 2;
	}
	if (!ip_is_ipv4_literal(cli->bind_override))
	{
		fprintf(stderr, "bind address '%s' is not an IPv4 address like 127.0.0.1\n",
			cli->bind_override);
		return 2;
	}
	strcpy(config->bind_address, cli->bind_override);
	return 0;
}

static u32
speed_to_baud(speed_t speed)
{
	switch (speed)
	{
#ifdef B50
	case B50: return 50;
#endif
#ifdef B75
	case B75: return 75;
#endif
#ifdef B110
	case B110: return 110;
#endif
#ifdef B134
	case B134: return 134;
#endif
#ifdef B150
	case B150: return 150;
#endif
#ifdef B200
	case B200: return 200;
#endif
#ifdef B300
	case B300: return 300;
#endif
#ifdef B600
	case B600: return 600;
#endif
#ifdef B1200
	case B1200: return 1200;
#endif
#ifdef B1800
	case B1800: return 1800;
#endif
#ifdef B2400
	case B2400: return 2400;
#endif
#ifdef B4800
	case B4800: return 4800;
#endif
#ifdef B9600
	case B9600: return 9600;
#endif
#ifdef B19200
	case B19200: return 19200;
#endif
#ifdef B38400
	case B38400: return 38400;
#endif
#ifdef B57600
	case B57600: return 57600;
#endif
#ifdef B115200
	case B115200: return 115200;
#endif
#ifdef B230400
	case B230400: return 230400;
#endif
#ifdef B460800
	case B460800: return 460800;
#endif
#ifdef B500000
	case B500000: return 500000;
#endif
#ifdef B576000
	case B576000: return 576000;
#endif
#ifdef B921600
	case B921600: return 921600;
#endif
#ifdef B1000000
	case B1000000: return 1000000;
#endif
#ifdef B1152000
	case B1152000: return 1152000;
#endif
#ifdef B1500000
	case B1500000: return 1500000;
#endif
#ifdef B2000000
	case B2000000: return 2000000;
#endif
#ifdef B2500000
	case B2500000: return 2500000;
#endif
#ifdef B3000000
	case B3000000: return 3000000;
#endif
#ifdef B3500000
	case B3500000: return 3500000;
#endif
#ifdef B4000000
	case B4000000: return 4000000;
#endif
	default: return 0;
	}
}

static bool
baud_to_speed(u32 baud, speed_t* out)
{
	switch (baud)
	{
#ifdef B50
	case 50: *out = B50; return true;
#endif
#ifdef B75
	case 75: *out = B75; return true;
#endif
#ifdef B110
	case 110: *out = B110; return true;
#endif
#ifdef B134
	case 134: *out = B134; return true;
#endif
#ifdef B150
	case 150: *out = B150; return true;
#endif
#ifdef B200
	case 200: *out = B200; return true;
#endif
#ifdef B300
	case 300: *out = B300; return true;
#endif
#ifdef B600
	case 600: *out = B600; return true;
#endif
#ifdef B1200
	case 1200: *out = B1200; return true;
#endif
#ifdef B1800
	case 1800: *out = B1800; return true;
#endif
#ifdef B2400
	case 2400: *out = B2400; return true;
#endif
#ifdef B4800
	case 4800: *out = B4800; return true;
#endif
#ifdef B9600
	case 9600: *out = B9600; return true;
#endif
#ifdef B19200
	case 19200: *out = B19200; return true;
#endif
#ifdef B38400
	case 38400: *out = B38400; return true;
#endif
#ifdef B57600
	case 57600: *out = B57600; return true;
#endif
#ifdef B115200
	case 115200: *out = B115200; return true;
#endif
#ifdef B230400
	case 230400: *out = B230400; return true;
#endif
#ifdef B460800
	case 460800: *out = B460800; return true;
#endif
#ifdef B500000
	case 500000: *out = B500000; return true;
#endif
#ifdef B576000
	case 576000: *out = B576000; return true;
#endif
#ifdef B921600
	case 921600: *out = B921600; return true;
#endif
#ifdef B1000000
	case 1000000: *out = B1000000; return true;
#endif
#ifdef B1152000
	case 1152000: *out = B1152000; return true;
#endif
#ifdef B1500000
	case 1500000: *out = B1500000; return true;
#endif
#ifdef B2000000
	case 2000000: *out = B2000000; return true;
#endif
#ifdef B2500000
	case 2500000: *out = B2500000; return true;
#endif
#ifdef B3000000
	case 3000000: *out = B3000000; return true;
#endif
#ifdef B3500000
	case 3500000: *out = B3500000; return true;
#endif
#ifdef B4000000
	case 4000000: *out = B4000000; return true;
#endif
	default: return false;
	}
}

static bool
tty_is_pty_slave(int fd)
{
	char	name[64];

	if (ttyname_r(fd, name, sizeof(name)) != 0)
		return false;
	return strncmp(name, "/dev/pts/", 9) == 0;
}

static u32
tty_baud(int fd)
{
	struct termios	tio;

	if (!isatty(fd) || tty_is_pty_slave(fd))
		return 0;
	if (tcgetattr(fd, &tio) != 0)
		return 0;
	return speed_to_baud(cfgetospeed(&tio));
}

/* Applies a known raw 8N1 serial configuration. When baud is 0, preserves the
 * existing baud rate so a configured serial line keeps its speed. Non-tty fds
 * are skipped. */
static bool
set_tty_raw(int fd, u32 baud)
{
	struct termios	tio;
	speed_t		ispeed;
	speed_t		ospeed;
	speed_t		requested = B9600;

	if (!isatty(fd))
		return true;
	if (tcgetattr(fd, &tio) != 0)
		return false;
	if (baud > 0 && !baud_to_speed(baud, &requested))
	{
		errno = EINVAL;
		return false;
	}

	ispeed = cfgetispeed(&tio);
	ospeed = cfgetospeed(&tio);
	cfmakeraw(&tio);
	if (baud > 0)
	{
		ispeed = requested;
		ospeed = requested;
	}
	cfsetispeed(&tio, ispeed);
	cfsetospeed(&tio, ospeed);
	tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	tio.c_cflag |= CS8 | CLOCAL | CREAD;
	tio.c_iflag &= ~(IXON | IXOFF | IXANY);
#ifdef ECHOE
	tio.c_lflag &= ~ECHOE;
#endif
#ifdef ECHOK
	tio.c_lflag &= ~ECHOK;
#endif
#ifdef ECHONL
	tio.c_lflag &= ~ECHONL;
#endif
#ifdef ECHOCTL
	tio.c_lflag &= ~ECHOCTL;
#endif
#ifdef ECHOKE
	tio.c_lflag &= ~ECHOKE;
#endif
#ifdef ECHOPRT
	tio.c_lflag &= ~ECHOPRT;
#endif
#ifdef NOKERNINFO
	tio.c_lflag &= ~NOKERNINFO;
#endif
	if (tcsetattr(fd, TCSANOW, &tio) != 0)
		return false;
	return true;
}

static s32
run_rtu(const cli_t* cli, config_t* config)
{
	int	fd       = -1;
	int	slave_fd = -1;
	s32	status   = 1;

	if (cli->rtu_pty)
	{
#if defined(__linux__)
		char	name[128];

		if (openpty(&fd, &slave_fd, name, NULL, NULL) < 0)
		{
			fprintf(stderr, "cannot create PTY: %s\n", strerror(errno));
			return 1;
		}
		if (!set_tty_raw(slave_fd, config->baud))
		{
			fprintf(stderr, "cannot configure RTU PTY slave: %s\n", strerror(errno));
			close(slave_fd);
			close(fd);
			return 1;
		}
		/* PTY has no real UART baud; keep baud unknown unless explicitly configured. */
		printf("RTU PTY slave: %s\n", name);
		fflush(stdout);
#else
		fprintf(stderr, "--rtu-pty is not supported on this platform\n");
		return 2;
#endif
	}
	else
	{
		fd = open(cli->rtu_device, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
		{
			fprintf(stderr, "cannot open RTU device '%s': %s\n",
				cli->rtu_device, strerror(errno));
			return 1;
		}
	}

	if (!set_tty_raw(fd, config->baud))
	{
		fprintf(stderr, "cannot configure RTU device '%s': %s\n",
			cli->rtu_pty ? "pty" : cli->rtu_device, strerror(errno));
		goto cleanup;
	}
	{
		int	flags = fcntl(fd, F_GETFL, 0);

		if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		{
			fprintf(stderr, "cannot set RTU fd non-blocking: %s\n", strerror(errno));
			goto cleanup;
		}
	}
	if (config->baud == 0)
		config->baud = tty_baud(fd);

	{
		rtu_server_options_t	options = {
			.fd           = fd,
			.seed         = config->seed,
			.frame_gap_ms = config->frame_gap_ms,
			.baud         = config->baud,
			.devices      = config->devices,
			.device_count = config->device_count,
		};

		status = rtu_server_run(&options);
	}

cleanup:
	if (slave_fd >= 0)
		close(slave_fd);
	if (fd >= 0)
		close(fd);
	return status;
}

static s32
run_tcp(config_t* config)
{
	tcp_server_options_t	options = {
		.bind_address    = config->bind_address,
		.port            = config->port,
		.seed            = config->seed,
		.idle_timeout_ms = config->idle_timeout_ms,
		.devices         = config->devices,
		.device_count    = config->device_count,
	};

	return tcp_server_run(&options);
}

int
main(int argc, char** argv)
{
	cli_t		cli      = { 0 };
	config_t	config   = { 0 };
	char		error[512];
	arena_t*	app      = NULL;
	arena_t*	scratch  = NULL;
	bool		rtu_mode = false;
	const char*	rtu_path = NULL;
	s32		status   = 1;

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

	status = apply_overrides(&cli, &config, cli.config_path);
	if (status != 0)
		goto cleanup;

	rtu_mode = cli.rtu_pty || cli.rtu_device;
	rtu_path = cli.rtu_device ? cli.rtu_device : (cli.rtu_pty ? "pty" : NULL);
	print_summary(&config, rtu_mode, rtu_path);
	if (cli.check_only)
	{
		puts("config OK");
		status = 0;
		goto cleanup;
	}

	status = rtu_mode ? run_rtu(&cli, &config) : run_tcp(&config);

cleanup:
	if (scratch)
		arena_release(scratch);
	if (app)
		arena_release(app);
	return status;
}
