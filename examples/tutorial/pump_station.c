/**
 * @file
 * @brief Tutorial program: a water pumping station outstation embedded in a test harness.
 *
 * Builds the pumping-station device in code, serves it over Modbus TCP from the program's own loop,
 * simulates the process (tank level, pump, alarms, run time) from the host side, guards client
 * writes with an access hook, keeps an event log in file records and a level history in a FIFO,
 * and shuts down on Ctrl-C. `--self-test` makes the program its own Modbus client and exits 0 when
 * every check passes. Only "mogbus.h" is included from the library; see
 * docs/user/library/library-start.md.
 */
#include "mogbus.h"
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** @brief Addresses of the scenario, shared by the device, the hook and the self-test. */
enum
{
	HOLD_SETPOINT   = 0,	/* float32, metres */
	HOLD_MODE       = 2,	/* u16 */
	HOLD_SITE       = 10,	/* string, 8 registers */
	COIL_RUN        = 0,
	DISC_HIGH       = 0,
	DISC_LOW        = 1,
	DISC_RUNNING    = 2,
	IN_LEVEL        = 0,	/* float32, metres */
	IN_OUTFLOW      = 2,	/* float32, m3/h */
	IN_CURRENT      = 4,	/* u16, 0.1 A */
	IN_INLET_TEMP   = 6,	/* float32, ramp */
	IN_RUN_SECONDS  = 10,	/* u32 */
	IN_CLOCK_MINUTE = 20,	/* u16, wall clock */
	FIFO_ADDRESS    = 0,
	LOG_FILE        = 1,	/* file 1: record 1 = event counter, records 2-100 = ring of codes */
	LOG_RECORDS     = 100,
	FIFO_MAX        = 31,
	FC_STATUS_BLOCK = 0x41,	/* vendor function code: read the station status block */
};

/** @brief Values of the mode register. */
enum
{
	MODE_OFF,
	MODE_AUTO,
	MODE_MANUAL,
};

/** @brief Event codes written to the log file: high byte = what, low byte = value. */
enum
{
	EVT_HIGH     = 0x0100,
	EVT_LOW      = 0x0200,
	EVT_PUMP     = 0x0300,
	EVT_MODE     = 0x0400,
	EVT_SETPOINT = 0x0500,
};

/** @brief The simulated process and what the access hook counts. */
struct station
{
	mogbus_device_t*	dev;
	double			level;			/**< tank level, m */
	double			run_s;			/**< pump run time, s */
	double			sample_s;		/**< time since the last FIFO sample */
	bool			pump;
	bool			high;
	bool			low;
	uint16_t		samples[FIFO_MAX];	/**< level history in cm, newest last */
	size_t			sample_count;
	uint32_t		events;
	unsigned		refused;		/**< writes vetoed by the hook */
	unsigned		accepted;		/**< writes the hook let through */
};

/** @brief Where the signal handler finds the server (see mogbus_stop_on_signals). */
static mogbus_server_t* volatile	g_server;
static unsigned				g_warnings;
static int				g_failed;

/** @brief Log sink: prefixes the library's lines and counts warnings. */
static void
log_sink(void* user, mogbus_log_level_t level, const char* message)
{
	(void)user;
	if (level >= MOGBUS_LOG_WARN)
		g_warnings++;
	fprintf(stderr, "  [mogbus %s] %s\n",
		level == MOGBUS_LOG_DEBUG ? "debug" : level == MOGBUS_LOG_INFO ? "info" : "warn", message);
}

/** @brief Decodes two registers as a float32, high word first. */
static double
float_of(const uint16_t* regs)
{
	uint32_t	raw = ((uint32_t)regs[0] << 16) | regs[1];
	float		f;

	memcpy(&f, &raw, sizeof(f));
	return f;
}

/** @brief Appends an event to the file-record log (host side, so the access hook is not called). */
static void
log_event(struct station* st, uint16_t code)
{
	uint16_t slot = (uint16_t)(2 + st->events % (LOG_RECORDS - 1));
	uint16_t count = (uint16_t)++st->events;

	mogbus_device_write_file_records(st->dev, LOG_FILE, slot, 1, &code);
	mogbus_device_write_file_records(st->dev, LOG_FILE, 1, 1, &count);
}

/** @brief Builds the device of the scenario. The failure text is in mogbus_last_error(ctx). */
static mogbus_error_t
build_device(mogbus_t* ctx, struct station* st, uint8_t unit)
{
	const mogbus_point_t points[] = {
		{ .space = MOGBUS_HOLDING, .address = HOLD_SETPOINT, .type = MOGBUS_TYPE_F32, .value = 2.5 },
		{ .space = MOGBUS_HOLDING, .address = HOLD_MODE, .type = MOGBUS_TYPE_U16, .value = MODE_AUTO },
		{ .space = MOGBUS_HOLDING, .address = HOLD_SITE, .type = MOGBUS_TYPE_STRING, .length = 8,
		  .text = "NORTH-01", .read_only = true },
		{ .space = MOGBUS_COIL, .address = COIL_RUN, .type = MOGBUS_TYPE_BOOL },
		{ .space = MOGBUS_DISCRETE, .address = DISC_HIGH, .type = MOGBUS_TYPE_BOOL },
		{ .space = MOGBUS_DISCRETE, .address = DISC_LOW, .type = MOGBUS_TYPE_BOOL },
		{ .space = MOGBUS_DISCRETE, .address = DISC_RUNNING, .type = MOGBUS_TYPE_BOOL },
		{ .space = MOGBUS_INPUT, .address = IN_LEVEL, .type = MOGBUS_TYPE_F32, .value = 2.4 },
		{ .space = MOGBUS_INPUT, .address = IN_OUTFLOW, .type = MOGBUS_TYPE_F32 },
		{ .space = MOGBUS_INPUT, .address = IN_CURRENT, .type = MOGBUS_TYPE_U16 },
		/* Not simulated by the host: the library moves these by itself. */
		{ .space = MOGBUS_INPUT, .address = IN_INLET_TEMP, .type = MOGBUS_TYPE_F32, .kind = MOGBUS_POINT_RAMP,
		  .ramp = { .from = 12, .to = 18, .step = 0.5, .every_ms = 1000, .mode = MOGBUS_RAMP_BOUNCE } },
		{ .space = MOGBUS_INPUT, .address = IN_RUN_SECONDS, .type = MOGBUS_TYPE_U32 },
		{ .space = MOGBUS_INPUT, .address = IN_CLOCK_MINUTE, .type = MOGBUS_TYPE_U16, .kind = MOGBUS_POINT_CLOCK,
		  .clock_field = MOGBUS_CLOCK_MINUTE, .clock_utc = true },
	};
	uint16_t zeros[LOG_RECORDS] = { 0 };

	mogbus_error_t error;

	st->dev = mogbus_add_device(ctx, unit, "Pump station");
	if (!st->dev)
		return mogbus_last_error_code(ctx);
	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++)
	{
		error = mogbus_device_add_point(st->dev, &points[i]);
		if (error != MOGBUS_OK)
			return error;
	}
	st->level = 2.4;
	error = mogbus_device_add_file_record(st->dev, LOG_FILE, 1, LOG_RECORDS, zeros);
	return error != MOGBUS_OK ? error : mogbus_device_add_fifo(st->dev, FIFO_ADDRESS, 0, NULL);
}

/**
 * @brief Access hook: runs before every Modbus client read or write of the device.
 *
 * Refuses writes while the station is off (except to the mode register), setpoints outside
 * 0.5-4.5 m (or written as half a float) and unknown modes; keeps the event log read-only for
 * clients; refreshes the FIFO just before a client reads it.
 */
static uint8_t
on_access(void* user, mogbus_device_t* dev, const mogbus_access_t* a)
{
	struct station*	st = user;
	double		mode = MODE_AUTO;

	if (a->target == MOGBUS_TARGET_FIFO)
	{
		mogbus_device_set_fifo(dev, (uint16_t)a->address, (uint16_t)st->sample_count, st->samples);
		return 0;
	}
	if (a->target == MOGBUS_TARGET_FILE_RECORD)
		return a->kind == MOGBUS_ACCESS_WRITE ? 1 : 0;	/* the log is the host's */
	if (a->target != MOGBUS_TARGET_TABLE || a->kind != MOGBUS_ACCESS_WRITE)
		return 0;

	mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_MODE, &mode);
	if (mode == MODE_OFF &&
	    !(a->space == MOGBUS_HOLDING && a->address == HOLD_MODE && a->count == 1))
	{
		st->refused++;
		return 4;			/* server device failure: the station is off */
	}
	if (a->space == MOGBUS_HOLDING)
	{
		uint32_t end = a->address + a->count;
		bool	 has_hi = a->address <= HOLD_SETPOINT && end > HOLD_SETPOINT;
		bool	 has_lo = a->address <= HOLD_SETPOINT + 1 && end > HOLD_SETPOINT + 1;

		if (has_hi != has_lo)
		{
			st->refused++;
			return 3;		/* half a float32 */
		}
		if (has_hi)
		{
			double sp = float_of(a->values + (HOLD_SETPOINT - a->address));

			if (!(sp >= 0.5 && sp <= 4.5))
			{
				st->refused++;
				return 3;
			}
		}
		if (a->address <= HOLD_MODE &&
		    end > HOLD_MODE &&
		    a->values[HOLD_MODE - a->address] > MODE_MANUAL)
		{
			st->refused++;
			return 3;
		}
	}
	return 0;
}

/**
 * @brief Post-write hook: runs after a client write was applied and every rule let it through.
 *
 * Counts and logs the write and puts setpoint and mode changes in the event log, so the host reacts
 * to them without polling.
 */
static void
on_written(void* user, mogbus_device_t* dev, const mogbus_access_t* a)
{
	struct station*	st = user;
	uint32_t	end = a->address + a->count;

	(void)dev;
	if (a->target != MOGBUS_TARGET_TABLE)
		return;
	st->accepted++;
	printf("  client write: FC %02X, %s address %u, %u item(s)\n", a->function_code,
	       a->space == MOGBUS_COIL ? "coil" : "holding", a->address, a->count);
	if (a->space != MOGBUS_HOLDING)
		return;
	if (a->address <= HOLD_SETPOINT && end > HOLD_SETPOINT)
		log_event(st, EVT_SETPOINT | (uint16_t)(float_of(a->values + (HOLD_SETPOINT - a->address)) * 10));
	if (a->address <= HOLD_MODE && end > HOLD_MODE)
		log_event(st, EVT_MODE | a->values[HOLD_MODE - a->address]);
}

/** @brief Moves the process forward by dt seconds and publishes it with host-side writes. */
static void
simulate(struct station* st, double dt)
{
	mogbus_device_t*	dev = st->dev;
	double			mode = MODE_AUTO;
	double			setpoint = 2.5;
	uint16_t		coil = 0;
	bool			pump = st->pump;

	mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_MODE, &mode);
	mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_SETPOINT, &setpoint);
	mogbus_device_read(dev, MOGBUS_COIL, COIL_RUN, 1, &coil);

	if (mode == MODE_OFF)
		pump = false;
	else if (mode == MODE_MANUAL)
		pump = coil != 0;
	else if (st->level > setpoint + 0.2)
		pump = true;
	else if (st->level < setpoint - 0.2)
		pump = false;
	if (pump != st->pump)
		log_event(st, EVT_PUMP | pump);
	st->pump = pump;
	/* The coil is the client's run command: outside manual mode the host plays the PLC and
	 * mirrors the pump into it. mogbus_device_write behaves like a client write. */
	if (mode != MODE_MANUAL && coil != pump)
	{
		uint16_t v = pump;

		mogbus_device_write(dev, MOGBUS_COIL, COIL_RUN, 1, &v);
	}

	st->level += (0.02 - (pump ? 0.05 : 0.0)) * dt;
	st->level = st->level < 0 ? 0 : st->level > 5 ? 5 : st->level;
	if (pump)
		st->run_s += dt;
	if ((st->level > 4.0) != st->high)
		log_event(st, EVT_HIGH | (st->level > 4.0));
	if ((st->level < 0.5) != st->low)
		log_event(st, EVT_LOW | (st->level < 0.5));
	st->high = st->level > 4.0;
	st->low = st->level < 0.5;

	st->sample_s += dt;
	if (st->sample_s >= 1.0)
	{
		st->sample_s -= 1.0;
		if (st->sample_count == FIFO_MAX)
			memmove(st->samples, st->samples + 1, (FIFO_MAX - 1) * sizeof(st->samples[0]));
		else
			st->sample_count++;
		st->samples[st->sample_count - 1] = (uint16_t)(st->level * 100);
	}

	/* set_value makes a point host-owned: right for sensor readings clients must not write. */
	mogbus_device_set_value(dev, MOGBUS_INPUT, IN_LEVEL, st->level);
	mogbus_device_set_value(dev, MOGBUS_INPUT, IN_OUTFLOW, pump ? 120.0 : 0.0);
	mogbus_device_set_value(dev, MOGBUS_INPUT, IN_CURRENT, pump ? 85 : 0);
	mogbus_device_set_value(dev, MOGBUS_INPUT, IN_RUN_SECONDS, (double)(uint32_t)st->run_s);
	mogbus_device_set_value(dev, MOGBUS_DISCRETE, DISC_HIGH, st->high);
	mogbus_device_set_value(dev, MOGBUS_DISCRETE, DISC_LOW, st->low);
	mogbus_device_set_value(dev, MOGBUS_DISCRETE, DISC_RUNNING, pump);
}

static double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** @brief Asks the kernel for a free loopback port (the port is reused right away). */
static uint16_t
free_port(void)
{
	struct sockaddr_in	addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t		len  = sizeof(addr);
	int			fd   = socket(AF_INET, SOCK_STREAM, 0);
	uint16_t		port = 0;

	if (fd < 0)
		return 0;
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
	    getsockname(fd, (struct sockaddr*)&addr, &len) == 0)
		port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

/**
 * @brief Runs mogbus_probe in a child process while this one keeps stepping the server.
 *
 * The probe blocks, so the thread that steps the server cannot call it itself.
 */
static bool
probe_self(mogbus_server_t* server, const mogbus_server_options_t* options, uint8_t unit)
{
	pid_t	pid = fork();
	int	status = 0;

	if (pid < 0)
		return false;
	if (pid == 0)
	{
		bool ok = mogbus_probe(options, unit, 2000) == MOGBUS_OK;

		if (!ok)
			fprintf(stderr, "probe: %s\n", mogbus_last_error(NULL));
		_exit(ok ? 0 : 1);
	}
	for (int i = 0; i < 1000; i++)
	{
		if (waitpid(pid, &status, WNOHANG) == pid)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0;
		mogbus_server_step(server, 5);
	}
	return false;
}

/** @brief Sends one request PDU as a Modbus TCP frame and steps the server until the reply is in. */
static int
exchange(mogbus_server_t* server, int fd, uint8_t unit, const uint8_t* pdu, size_t pdu_len, uint8_t* reply)
{
	static uint16_t	tid;
	uint8_t		frame[260] = { 0 };
	size_t		got = 0;

	tid++;
	frame[0] = (uint8_t)(tid >> 8);
	frame[1] = (uint8_t)tid;
	frame[5] = (uint8_t)(pdu_len + 1);
	frame[6] = unit;
	memcpy(frame + 7, pdu, pdu_len);
	if (write(fd, frame, 7 + pdu_len) != (ssize_t)(7 + pdu_len))
		return -1;
	for (int i = 0; i < 2000; i++)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		if (mogbus_server_step(server, 1) != MOGBUS_STEP_CONTINUE)
			return -1;
		if (poll(&pfd, 1, 0) > 0)
		{
			ssize_t n = read(fd, reply + got, 260 - got);

			if (n <= 0)
				return -1;
			got += (size_t)n;
		}
		if (got >= 7 && got >= 6 + (size_t)(reply[4] << 8 | reply[5]))
			return (int)got - 7;		/* PDU length; the PDU starts at reply + 7 */
	}
	return -1;
}

static void
check(bool ok, const char* what)
{
	printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		g_failed++;
}

static bool
is_exception(const uint8_t* pdu, uint8_t fc, uint8_t code)
{
	return pdu[0] == (fc | 0x80) && pdu[1] == code;
}

static double
float_at(const uint8_t* p)
{
	uint16_t regs[2] = { (uint16_t)(p[0] << 8 | p[1]), (uint16_t)(p[2] << 8 | p[3]) };

	return float_of(regs);
}

static void
put_float(uint8_t* p, float f)
{
	uint32_t raw;

	memcpy(&raw, &f, sizeof(raw));
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(raw >> (24 - 8 * i));
}

/** @brief The program as its own Modbus client: returns 0 when every check passes. */
static int
self_test(mogbus_server_t* server, const mogbus_server_options_t* options, struct station* st, uint8_t unit)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(options->port),
				    .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int		fd = socket(AF_INET, SOCK_STREAM, 0);
	uint8_t		r[260];
	uint8_t*	pdu = r + 7;
	char		text[32];
	double		sp = 0;
	uint8_t		write_setpoint[] = { 0x10, 0, HOLD_SETPOINT, 0, 2, 4, 0, 0, 0, 0 };
	uint8_t		write_site[] = { 0x10, 0, HOLD_SITE, 0, 1, 2, 0, 0 };
	uint8_t		write_mode[] = { 0x06, 0, HOLD_MODE, 0, 0 };
	uint8_t		write_coil[] = { 0x05, 0, COIL_RUN, 0xFF, 0 };
	uint8_t		read_setpoint[] = { 0x03, 0, HOLD_SETPOINT, 0, 2 };
	uint8_t		read_site[] = { 0x03, 0, HOLD_SITE, 0, 8 };
	uint8_t		read_discrete[] = { 0x02, 0, 0, 0, 3 };
	uint8_t		read_level[] = { 0x04, 0, IN_LEVEL, 0, 2 };
	uint8_t		read_log[] = { 0x14, 7, 6, 0, LOG_FILE, 0, 1, 0, 1 };
	uint8_t		write_log[] = { 0x15, 9, 6, 0, LOG_FILE, 0, 2, 0, 1, 0, 9 };
	uint8_t		read_fifo[] = { 0x18, 0, FIFO_ADDRESS };
	uint8_t		read_status[] = { FC_STATUS_BLOCK, 1 };
	uint8_t		read_status_bad[] = { FC_STATUS_BLOCK, 9 };

	if (fd < 0 || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
	{
		perror("self-test connect");
		return 1;
	}
	puts("self-test:");
	check(probe_self(server, options, unit), "mogbus_probe answers from a child process");

	check(exchange(server, fd, unit, read_setpoint, sizeof(read_setpoint), r) == 6 && pdu[0] == 3 &&
	      float_at(pdu + 2) == 2.5, "FC 03 reads the setpoint, 2.5 m");
	check(exchange(server, fd, unit, read_site, sizeof(read_site), r) == 18 && memcmp(pdu + 2, "NORTH-01", 8) == 0,
	      "FC 03 reads the site name string");

	uint32_t	events_before = st->events;

	put_float(write_setpoint + 6, 9.0f);
	check(exchange(server, fd, unit, write_setpoint, sizeof(write_setpoint), r) == 2 && is_exception(pdu, 0x10, 3),
	      "FC 16 setpoint 9.0 m is refused by the hook: exception 03");
	check(st->events == events_before, "the refused write did not reach the post-write hook");
	put_float(write_setpoint + 6, 3.0f);
	check(exchange(server, fd, unit, write_setpoint, sizeof(write_setpoint), r) == 5 && pdu[0] == 0x10,
	      "FC 16 setpoint 3.0 m is accepted");
	check(st->events == events_before + 1, "the post-write hook logged the setpoint event");
	check(mogbus_device_get_value(st->dev, MOGBUS_HOLDING, HOLD_SETPOINT, &sp) && sp == 3.0,
	      "the host reads the new setpoint back");
	check(exchange(server, fd, unit, write_site, sizeof(write_site), r) == 2 && is_exception(pdu, 0x10, 2),
	      "FC 16 on the read-only site name: exception 02");

	write_mode[4] = 7;
	check(exchange(server, fd, unit, write_mode, sizeof(write_mode), r) == 2 && is_exception(pdu, 6, 3),
	      "FC 06 mode 7 is refused: exception 03");
	write_mode[4] = MODE_MANUAL;
	check(exchange(server, fd, unit, write_mode, sizeof(write_mode), r) == 5, "FC 06 switches to manual");
	check(exchange(server, fd, unit, write_coil, sizeof(write_coil), r) == 5, "FC 05 sets the run command");
	simulate(st, 1.0);
	check(exchange(server, fd, unit, read_discrete, sizeof(read_discrete), r) == 3 && pdu[1] == 1 && pdu[2] == 4,
	      "FC 02: only 'pump running' is set");

	write_mode[4] = MODE_OFF;
	check(exchange(server, fd, unit, write_mode, sizeof(write_mode), r) == 5, "FC 06 switches the station off");
	check(exchange(server, fd, unit, write_coil, sizeof(write_coil), r) == 2 && is_exception(pdu, 5, 4),
	      "FC 05 while off is refused: exception 04");
	for (int i = 0; i < 100; i++)
		simulate(st, 1.0);
	check(exchange(server, fd, unit, read_discrete, sizeof(read_discrete), r) == 3 && (pdu[2] & 1),
	      "100 s later the high-level alarm is set");
	check(exchange(server, fd, unit, read_level, sizeof(read_level), r) == 6 && float_at(pdu + 2) > 4.0,
	      "FC 04 reads a level above 4 m");

	check(exchange(server, fd, unit, read_log, sizeof(read_log), r) == 6 && pdu[0] == 0x14 &&
	      (pdu[4] << 8 | pdu[5]) >= 5, "FC 14 reads the event counter (at least 5 events)");
	check(exchange(server, fd, unit, write_log, sizeof(write_log), r) == 2 && is_exception(pdu, 0x15, 1),
	      "FC 15 on the event log is refused: exception 01");
	check(exchange(server, fd, unit, read_fifo, sizeof(read_fifo), r) > 4 && pdu[0] == 0x18 &&
	      (pdu[3] << 8 | pdu[4]) == FIFO_MAX, "FC 18 returns the last 31 level samples");

	check(exchange(server, fd, unit, read_status, sizeof(read_status), r) == 12 && pdu[0] == FC_STATUS_BLOCK &&
	      pdu[1] == 1 && pdu[2] == MODE_OFF && (pdu[3] & 1) && (pdu[4] << 8 | pdu[5]) > 400 &&
	      (pdu[8] << 8 | pdu[9]) == 3, "FC 0x41 returns the status block computed from the live values");
	check(exchange(server, fd, unit, read_status_bad, sizeof(read_status_bad), r) == 2 &&
	      is_exception(pdu, FC_STATUS_BLOCK, 3), "FC 0x41 with an unknown block id: exception 03");

	write_mode[4] = MODE_AUTO;
	check(exchange(server, fd, unit, write_mode, sizeof(write_mode), r) == 5, "FC 06 switches back to auto");
	check(mogbus_device_get_string(st->dev, MOGBUS_HOLDING, HOLD_SITE, text, sizeof(text)) &&
	      strcmp(text, "NORTH-01") == 0, "the host reads the site name with mogbus_device_get_string");
	check(st->refused == 3 && st->accepted >= 5, "the hook counted its vetoes and accepted writes");
	close(fd);
	return g_failed ? 1 : 0;
}

/**
 * @brief Vendor function code 0x41, "read station status block": the reply is computed from the live
 *        values when the request arrives, as a vendor PLC would.
 *
 * Request data: one byte, the block id (1). Reply data: block id, mode, flags (bit 0 high, 1 low,
 * 2 running), level in cm (u16), outflow in 0.1 m3/h (u16), refused and accepted writes (u16 each).
 */
static int
status_block(void* user, mogbus_device_t* dev, uint8_t unit_id, uint8_t function_code,
	     const uint8_t* data, size_t len, uint8_t* reply, size_t reply_cap, size_t* reply_len)
{
	const struct station*	st = user;
	double			mode = 0, level = 0, outflow = 0;
	uint16_t		fields[4];
	uint8_t			discrete[3] = { 0 };
	uint16_t		bits[3];

	(void)unit_id;
	(void)function_code;
	if (len != 1 ||
	    data[0] != 1 ||
	    reply_cap < 11)
		return 3;				/* exception 03: unknown block */
	mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_MODE, &mode);
	mogbus_device_get_value(dev, MOGBUS_INPUT, IN_LEVEL, &level);
	mogbus_device_get_value(dev, MOGBUS_INPUT, IN_OUTFLOW, &outflow);
	mogbus_device_read(dev, MOGBUS_DISCRETE, DISC_HIGH, 3, bits);
	for (int i = 0; i < 3; i++)
		discrete[0] |= (uint8_t)((bits[i] ? 1 : 0) << i);
	fields[0] = (uint16_t)(level * 100.0 + 0.5);
	fields[1] = (uint16_t)(outflow * 10.0 + 0.5);
	fields[2] = (uint16_t)st->refused;
	fields[3] = (uint16_t)st->accepted;
	reply[0] = 1;
	reply[1] = (uint8_t)mode;
	reply[2] = discrete[0];
	for (int i = 0; i < 4; i++)
	{
		reply[3 + 2 * i] = (uint8_t)(fields[i] >> 8);
		reply[4 + 2 * i] = (uint8_t)fields[i];
	}
	*reply_len = 11;
	return 0;
}

static void
print_metrics(const mogbus_server_t* server)
{
	static char	text[32768];
	char*		line;

	if (mogbus_server_metrics(server, text, sizeof(text)) == 0)
		return;
	puts("metrics:");
	for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n"))
		if (strncmp(line, "mogbus_requests_total", 21) == 0 ||
		    strncmp(line, "mogbus_device_exceptions_total", 30) == 0 ||
		    strncmp(line, "mogbus_connections_total", 24) == 0)
			printf("  %s\n", line);
}

static void
usage(void)
{
	puts("usage: pump_station [--self-test] [--config FILE] [--unit N] [--port N] [--http PORT]\n"
	     "                    [--speed X] [--seconds N] [--verbose]");
}

int
main(int argc, char** argv)
{
	struct station		st = { 0 };
	mogbus_t*		ctx = mogbus_create();
	mogbus_server_t*	server = NULL;
	mogbus_server_options_t	options = mogbus_server_options_defaults();
	const char*		config = NULL;
	bool			test = false;
	uint8_t			unit = 1;
	uint16_t		http_port = 0;
	double			speed = 10, limit = 0;
	int			result = 1;

	for (int i = 1; i < argc; i++)
	{
		bool more = i + 1 < argc;

		if (strcmp(argv[i], "--self-test") == 0)
			test = true;
		else if (strcmp(argv[i], "--verbose") == 0)
			mogbus_set_log_level(MOGBUS_LOG_DEBUG);
		else if (strcmp(argv[i], "--config") == 0 && more)
			config = argv[++i];
		else if (strcmp(argv[i], "--unit") == 0 && more)
			unit = (uint8_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--port") == 0 && more)
			options.port = (uint16_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--http") == 0 && more)
			http_port = (uint16_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--speed") == 0 && more)
			speed = atof(argv[++i]);
		else if (strcmp(argv[i], "--seconds") == 0 && more)
			limit = atof(argv[++i]);
		else
		{
			usage();
			mogbus_destroy(ctx);
			return 2;
		}
	}
	if (!ctx)
		return 1;
	setvbuf(stdout, NULL, _IOLBF, 0);
	mogbus_set_log_sink(log_sink, NULL);
	mogbus_stop_on_signals(&g_server);
	printf("mogbus %s pump station\n", mogbus_version());

	if (config)
	{
		mogbus_server_options_t from_config;

		if (mogbus_load_config(ctx, config) != MOGBUS_OK)
		{
			fprintf(stderr, "config %s: %s\n", config, mogbus_last_error(ctx));
			goto done;
		}
		printf("loaded %s: %zu device(s)\n", config, mogbus_device_count(ctx));
		if (mogbus_config_server_options(ctx, &from_config))
		{
			uint16_t port = options.port;

			options = from_config;
			if (port)
				options.port = port;
		}
	}
	if (build_device(ctx, &st, unit) != MOGBUS_OK)
	{
		fprintf(stderr, "device: %s\n", mogbus_last_error(ctx));
		goto done;
	}
	{
		mogbus_device_info_t info;

		mogbus_device_info(st.dev, &info);
		printf("device \"%s\", unit %u: %u holding, %u input, %u coil, %u discrete points\n", info.name,
		       info.unit_id, info.point_count[MOGBUS_HOLDING], info.point_count[MOGBUS_INPUT],
		       info.point_count[MOGBUS_COIL], info.point_count[MOGBUS_DISCRETE]);
	}
	mogbus_device_set_access_hook(st.dev, on_access, &st);
	mogbus_device_set_written_hook(st.dev, on_written, &st);
	if (!mogbus_device_add_function_code(st.dev, FC_STATUS_BLOCK, status_block, &st))
	{
		fprintf(stderr, "cannot define function code 0x%02X\n", FC_STATUS_BLOCK);
		goto done;
	}

	if (test)
	{
		options.mode = MOGBUS_MODE_TCP;
		options.bind_address = "127.0.0.1";
		options.port = free_port();
		http_port = 0;
	}
	if (http_port)
	{
		options.http_bind_address = "127.0.0.1";
		options.http_port = http_port;
		options.http_ui = true;
	}
	if (mogbus_server_check(ctx, &options) != MOGBUS_OK)
	{
		fprintf(stderr, "options: %s\n", mogbus_last_error(ctx));
		goto done;
	}
	server = mogbus_server_create(ctx, &options);
	if (!server)
	{
		fprintf(stderr, "server (status %d): %s\n", (int)mogbus_last_error_code(ctx),
			mogbus_last_error(ctx));
		goto done;
	}
	g_server = server;
	printf("serving %zu device(s) on %s:%u%s\n", mogbus_device_count(ctx),
	       options.bind_address ? options.bind_address : "127.0.0.1", options.port,
	       http_port ? ", web UI on the HTTP port" : "");

	if (test)
	{
		result = self_test(server, &options, &st, unit);
		if (config)
			check(probe_self(server, &options, 2), "the config's device (unit 2) is served too");
		printf("self-test: %s\n", g_failed ? "FAILED" : "all checks passed");
		result = g_failed ? 1 : 0;
	}
	else
	{
		double start = now_s(), last = start, report = start;

		if (probe_self(server, &options, unit))
			puts("self-check: mogbus_probe answered");
		else
			fprintf(stderr, "self-check probe failed\n");
		puts("press Ctrl-C to stop");
		/* The host owns the loop: serve for up to 100 ms, then advance the process. */
		while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
		{
			double t = now_s();

			simulate(&st, (t - last) * speed);
			last = t;
			if (t - report >= 2)
			{
				report = t;
				printf("level %.2f m, pump %s, events %u, client writes %u ok / %u refused\n", st.level,
				       st.pump ? "ON" : "off", st.events, st.accepted, st.refused);
			}
			if (limit > 0 && t - start >= limit)
				break;
		}
		puts("stopping");
		result = 0;
	}
	print_metrics(server);
	if (g_warnings)
		printf("%u warning(s) logged\n", g_warnings);

done:
	g_server = NULL;
	mogbus_server_close(server);
	mogbus_destroy(ctx);
	return result;
}
