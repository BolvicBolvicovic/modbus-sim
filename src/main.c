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
#include "net/rtu_framer.h"
#include "net/rtu_server.h"
#include "net/rtu_tcp_server.h"
#include "net/tcp_server.h"
#include "util/baud.h"
#include "util/log.h"
#include "util/serial.h"
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
	     "(server (mode rtu-pty|rtu-device) ...). ASCII uses 8N1 over a byte stream\n"
	     "(PTY or device) selected by (mode ascii-pty|ascii-device); it has no TCP form.\n"
	     "ASCII frames are ':' + hex + LRC + CRLF, delimited by CRLF, not by timing.\n"
	     "RTU-over-TCP serves Modbus RTU frames on a TCP listen socket (separate transport).\n"
	     "Frame boundaries use Modbus t3.5 when baud is known, or frame-gap-ms/default.\n"
	     "t1.5 discard and unit-0 broadcast execution are opt-in via (server ...).\n"
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
			printf("%u device%s on %s:%u\n", config->device_count,
			       config->device_count == 1 ? "" : "s",
			       config->bind_address, config->port);
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


static bool
tty_is_pty(int fd)
{
	char		name[64];
#if defined(__linux__)
	unsigned int	ptn = 0;
#endif

	if (!isatty(fd))
		return false;
	if (ttyname_r(fd, name, sizeof(name)) == 0)
	{
		if (strcmp(name, "/dev/ptmx") == 0)
			return true;
		if (strncmp(name, "/dev/pts/", 9) == 0)
			return true;
	}
#if defined(__linux__)
	/* Fallback for master fds whose ttyname_r() may be non-PTS. */
	if (ioctl(fd, TIOCGPTN, &ptn) == 0)
		return true;
#endif
	return false;
}

static u32
tty_baud(int fd)
{
	struct termios	tio;

	if (!isatty(fd) || tty_is_pty(fd))
		return 0;
	if (tcgetattr(fd, &tio) != 0)
		return 0;
	return baud_from_speed(cfgetospeed(&tio));
}

/* Applies raw mode with the selected parity and stop bits. When baud is 0,
 * preserves the existing baud rate so a configured serial line keeps its
 * speed. Non-tty fds are skipped. */
static bool
set_tty_raw(int fd, u32 baud, parity_t parity, u32 stop_bits)
{
	struct termios	tio;
	speed_t		ispeed;
	speed_t		ospeed;
	speed_t		requested = B9600;
	tcflag_t	clear = 0;
	tcflag_t	set = 0;

	if (!isatty(fd))
		return true;
	if (!serial_line_flags(parity, stop_bits, &clear, &set))
	{
		errno = EOPNOTSUPP;
		return false;
	}
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
	tio.c_cflag &= ~(clear | CRTSCTS);
	tio.c_cflag |= set | CLOCAL | CREAD;
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
run_rtu(config_t* config)
{
	int	fd       = -1;
	int	slave_fd = -1;
	s32	status   = 1;

	if (config->mode == SERVER_MODE_RTU_PTY)
	{
#if defined(__linux__)
		char	name[128];

		if (openpty(&fd, &slave_fd, name, NULL, NULL) < 0)
		{
			fprintf(stderr, "cannot create PTY: %s\n", strerror(errno));
			return 1;
		}
		if (!set_tty_raw(slave_fd, config->baud, config->parity, config->stop_bits))
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
		fprintf(stderr, "RTU PTY mode is not supported on this platform\n");
		return 2;
#endif
	}
	else
	{
		fd = open(config->path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
		{
			fprintf(stderr, "cannot open RTU device '%s': %s\n",
				config->path, strerror(errno));
			return 1;
		}
	}

	if (!set_tty_raw(fd, config->baud, config->parity, config->stop_bits))
	{
		fprintf(stderr, "cannot configure RTU device '%s': %s\n",
			config->mode == SERVER_MODE_RTU_PTY ? "pty" : config->path,
			strerror(errno));
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
	/* PTY master/slave have no real UART baud; only auto-detect on real tty devices. */
	if (config->baud == 0 && config->mode == SERVER_MODE_RTU_DEVICE)
		config->baud = tty_baud(fd);

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

cleanup:
	if (slave_fd >= 0)
		close(slave_fd);
	if (fd >= 0)
		close(fd);
	return status;
}

static s32
run_ascii(config_t* config)
{
	int	fd       = -1;
	int	slave_fd = -1;
	s32	status   = 1;

	if (config->mode == SERVER_MODE_ASCII_PTY)
	{
#if defined(__linux__)
		char	name[128];

		if (openpty(&fd, &slave_fd, name, NULL, NULL) < 0)
		{
			fprintf(stderr, "cannot create PTY: %s\n", strerror(errno));
			return 1;
		}
		if (!set_tty_raw(slave_fd, config->baud, PARITY_NONE, 1))
		{
			fprintf(stderr, "cannot configure ASCII PTY slave: %s\n", strerror(errno));
			close(slave_fd);
			close(fd);
			return 1;
		}
		printf("ASCII PTY slave: %s\n", name);
		fflush(stdout);
#else
		fprintf(stderr, "ASCII PTY mode is not supported on this platform\n");
		return 2;
#endif
	}
	else
	{
		fd = open(config->path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
		{
			fprintf(stderr, "cannot open ASCII device '%s': %s\n",
				config->path, strerror(errno));
			return 1;
		}
	}

	if (!set_tty_raw(fd, config->baud, PARITY_NONE, 1))
	{
		fprintf(stderr, "cannot configure ASCII device '%s': %s\n",
			config->mode == SERVER_MODE_ASCII_PTY ? "pty" : config->path,
			strerror(errno));
		goto cleanup;
	}
	{
		int	flags = fcntl(fd, F_GETFL, 0);

		if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		{
			fprintf(stderr, "cannot set ASCII fd non-blocking: %s\n", strerror(errno));
			goto cleanup;
		}
	}
	/* Baud sets only the serial line speed here; it never affects ASCII framing. */
	{
		ascii_server_options_t	options = {
			.fd           = fd,
			.seed         = config->seed,
			.devices      = config->devices,
			.device_count = config->device_count,
		};

		status = ascii_server_run(&options);
	}

cleanup:
	if (slave_fd >= 0)
		close(slave_fd);
	if (fd >= 0)
		close(fd);
	return status;
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
