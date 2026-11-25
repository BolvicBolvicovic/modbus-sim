/* End-to-end scripted-machine coverage against the real TCP server.
 * Phase 1 has no state register, so states are inferred from points:
 * idle     = trip marker off, discrete 0 off, input 0 == 0
 * starting = trip marker off, discrete 0 off, 0 < input 0 < holding 0
 * running  = trip marker off, discrete 0 on, input 0 == holding 0
 * tripped  = trip marker on, discrete 0 off, input 0 == 0 (coil 0 may stay on)
 * A reset is confirmed by the machine restarting when coil 0 is set again. */
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "tests/tests_utils/test_support.h"
#include "util/clock.h"
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define UNIT_ID			7
#define COIL_START		0
#define COIL_RESET		1
#define DISCRETE_RUNNING	0
#define DISCRETE_TRIPPED	1
#define INPUT_SPEED		0
#define HOLDING_SETPOINT	0
#define SETPOINT		1200
#define TRIP_MIN_MS		600
#define TRIP_MAX_MS		900
#define RECV_TIMEOUT_MS		1000
#define STATE_TIMEOUT_MS	3000
#define POLL_SLEEP_US		5000
/* Clear 250 ms into running: 350 ms before TRIP_MIN_MS. */
#define PRIORITY_CLEAR_DELAY_US	250000
/* Read 1000 ms after clearing: 350 ms past TRIP_MAX_MS. */
#define PRIORITY_SETTLE_US	1000000

typedef struct client_t client_t;
struct client_t
{
	int	fd;
};

typedef struct observation_t observation_t;
struct observation_t
{
	bool	start;
	bool	running;
	bool	tripped;
	u16	speed;
};

static ts_server_t g_server = {
	.pid		= -1,
	.master_fd	= -1,
	.probe_family	= AF_INET,
	.flags		= TS_CAPTURE_STDERR | TS_NULL_STDOUT | TS_CLEAN_EXIT,
	.label		= "machine e2e",
	.ready_needle	= "listening on"
};

static void
fixture_cleanup(void)
{
	ts_server_cleanup(&g_server);
}

static bool
fixture_start(void)
{
	char		config[2048];
	const char*	bin = getenv("MOGBUS_BIN");
	char*		argv[] = { (char*)(bin ? bin : "./mogbus"), g_server.config, NULL };
	int		n;

	g_server.probe_port = ts_free_port();
	if (!g_server.probe_port)
		return false;
	n = snprintf(config, sizeof(config),
		     "(server (mode tcp) (port %u) (bind \"127.0.0.1\") (seed 12345))\n"
		     "(device (unit-id 7)\n"
		     "  (coil 0) (coil 1)\n"
		     "  (discrete 0) (discrete 1)\n"
		     "  (input 0)\n"
		     "  (holding 0 (static 1200))\n"
		     "  (machine motor (initial idle)\n"
		     "    (var speed 0)\n"
		     "    (state idle\n"
		     "      (on-enter (set! speed 0) (set! (discrete 0) #f)\n"
		     "                 (set! (discrete 1) #f) (set! (input 0) 0)))\n"
		     "    (state starting\n"
		     "      (on-enter (set! speed 0) (set! (discrete 0) #f)\n"
		     "                 (set! (discrete 1) #f) (set! (input 0) 0))\n"
		     "      (on-tick (when (< speed (holding 0))\n"
		     "                 (set! speed (min (+ speed 300) (holding 0)))\n"
		     "                 (set! (input 0) speed))))\n"
		     "    (state running\n"
		     "      (on-enter (set! (discrete 0) #t) (set! (discrete 1) #f)\n"
		     "                 (set! (input 0) (holding 0))))\n"
		     "    (state tripped\n"
		     "      (on-enter (set! (discrete 0) #f) (set! (discrete 1) #t)\n"
		     "                 (set! (input 0) 0)))\n"
		     "    (transition idle starting (when (coil 0)))\n"
		     "    (transition starting running (after-ms 200))\n"
		     "    (transition running tripped (after-ms (random %d %d)))\n"
		     "    (transition running idle (when (not (coil 0))))\n"
		     "    (transition tripped idle (when (coil 1)))))\n",
		     g_server.probe_port, TRIP_MIN_MS, TRIP_MAX_MS);
	if (n <= 0 || (size_t)n >= sizeof(config) ||
	    !ts_write_config(&g_server, "/tmp/mogbus-machine-e2e-XXXXXX", config))
		return false;
	return ts_server_start(&g_server, argv);
}

static bool
fixture_stop(void)
{
	bool ok = ts_server_stop(&g_server);

	g_server.probe_port = 0;
	return ok;
}

SETUP_SUITE()
{
	(void)_ss_arena;
	atexit(fixture_cleanup);
}

PRE_TEST()
{
	(void)fixture_stop();
}

static bool
client_open(client_t* client)
{
	client->fd = ts_connect_loopback(g_server.probe_port, AF_INET, RECV_TIMEOUT_MS);
	return client->fd >= 0;
}

static void
client_close(client_t* client)
{
	if (client->fd >= 0)
	{
		close(client->fd);
		client->fd = -1;
	}
}

static bool
exchange(client_t* client, const u8* req, u32 req_len, u8* resp, u32* resp_len)
{
	u16 tid = ts_next_tid();

	return ts_mbap_send(client->fd, UNIT_ID, tid, req, req_len) &&
	       ts_mbap_recv(client->fd, UNIT_ID, tid, resp, resp_len, RECV_TIMEOUT_MS);
}

static bool
read_register(client_t* client, u8 function, u16 address, u16* value)
{
	u8	req[5];
	u8	resp[MODBUS_MAX_PDU];
	u32	len = 0;

	req[0] = function;
	put_be16(req + 1, address);
	put_be16(req + 3, 1);
	if (!exchange(client, req, sizeof(req), resp, &len) ||
	    len != 4 || resp[0] != function || resp[1] != 2)
		return false;
	*value = get_be16(resp + 2);
	return true;
}

static bool
read_bit(client_t* client, u8 function, u16 address, bool* value)
{
	u8	req[5];
	u8	resp[MODBUS_MAX_PDU];
	u32	len = 0;

	req[0] = function;
	put_be16(req + 1, address);
	put_be16(req + 3, 1);
	if (!exchange(client, req, sizeof(req), resp, &len) ||
	    len != 3 || resp[0] != function || resp[1] != 1)
		return false;
	*value = (resp[2] & 1) != 0;
	return true;
}

static bool
write_coil(client_t* client, u16 address, bool value)
{
	u8	req[5];
	u8	resp[MODBUS_MAX_PDU];
	u32	len = 0;

	req[0] = FC_WRITE_SINGLE_COIL;
	put_be16(req + 1, address);
	req[3] = value ? 0xFF : 0x00;
	req[4] = 0x00;
	if (!exchange(client, req, sizeof(req), resp, &len) || len != 5)
		return false;
	return memcmp(resp, req, sizeof(req)) == 0;
}

/* Every read is also a request tick, so the first read can fire a transition
 * that the following reads then observe. */
static bool
observe(client_t* client, observation_t* obs)
{
	if (!read_bit(client, FC_READ_DISCRETE_INPUTS, DISCRETE_TRIPPED, &obs->tripped))
		return false;
	if (!read_bit(client, FC_READ_DISCRETE_INPUTS, DISCRETE_RUNNING, &obs->running))
		return false;
	if (!read_register(client, FC_READ_INPUT_REGISTERS, INPUT_SPEED, &obs->speed))
		return false;
	if (!read_bit(client, FC_READ_COILS, COIL_START, &obs->start))
		return false;
	return true;
}

/* Re-arm after the short starting window was missed: stop first, pulse reset
 * off/on/off to return from running or tripped to idle, then start again. */
static bool
rearm_start(client_t* client)
{
	if (!write_coil(client, COIL_START, false))
		return false;
	if (!write_coil(client, COIL_RESET, false))
		return false;
	if (!write_coil(client, COIL_RESET, true))
		return false;
	if (!write_coil(client, COIL_RESET, false))
		return false;
	return write_coil(client, COIL_START, true);
}

static bool
wait_starting(client_t* client, u16* speed_out)
{
	u64 deadline = clock_now_ms() + STATE_TIMEOUT_MS;

	for (;;)
	{
		observation_t obs = { 0 };

		if (!observe(client, &obs))
			return false;
		if (obs.start &&
		    !obs.running &&
		    !obs.tripped &&
		    obs.speed > 0 &&
		    obs.speed < SETPOINT)
		{
			*speed_out = obs.speed;
			return true;
		}
		if (clock_now_ms() >= deadline)
			return false;
		if (!rearm_start(client))
			return false;
		usleep(POLL_SLEEP_US);
	}
}

static bool
wait_running(client_t* client)
{
	u64 deadline = clock_now_ms() + STATE_TIMEOUT_MS;

	for (;;)
	{
		observation_t obs = { 0 };

		if (!observe(client, &obs))
			return false;
		if (obs.start &&
		    obs.running &&
		    !obs.tripped &&
		    obs.speed == SETPOINT)
			return true;
		if (clock_now_ms() >= deadline)
			return false;
		usleep(POLL_SLEEP_US);
	}
}

static bool
wait_tripped(client_t* client)
{
	u64 deadline = clock_now_ms() + STATE_TIMEOUT_MS;

	for (;;)
	{
		observation_t obs = { 0 };

		if (!observe(client, &obs))
			return false;
		if (obs.tripped &&
		    !obs.running &&
		    obs.speed == 0)
			return true;
		if (clock_now_ms() >= deadline)
			return false;
		usleep(POLL_SLEEP_US);
	}
}

/* Request ticks keep advancing the machine, so holding these values catches a
 * spurious restart or a premature reset. */
static bool
remains_at(client_t* client, const observation_t* expected, u32 duration_ms)
{
	u64 deadline = clock_now_ms() + duration_ms;

	while (clock_now_ms() < deadline)
	{
		observation_t obs = { 0 };

		if (!observe(client, &obs) ||
		    obs.start != expected->start ||
		    obs.running != expected->running ||
		    obs.tripped != expected->tripped ||
		    obs.speed != expected->speed)
			return false;
		usleep(POLL_SLEEP_US);
	}
	return true;
}

TEST(MachineE2EStartRunTripAndReset)
{
	client_t	client = { .fd = -1 };
	observation_t	expected = { 0 };
	u16		speed = 0;
	u16		setpoint = 0;
	bool		ok = fixture_start();

	if (ok)
		ok = client_open(&client);
	if (ok)
	{
		ok = read_register(&client, FC_READ_HOLDING_REGISTERS, HOLDING_SETPOINT,
				   &setpoint) && setpoint == SETPOINT;
	}
	if (ok)
	{
		expected.speed = 0;
		ok = remains_at(&client, &expected, 100);
	}
	if (ok)
		ok = write_coil(&client, COIL_START, true);
	if (ok)
		ok = wait_starting(&client, &speed);
	if (ok)
		ok = speed < setpoint;
	if (ok)
		ok = wait_running(&client);
	if (ok)
		ok = wait_tripped(&client);
	if (ok)
	{
		/* The stop guard stayed false here; the after-only trip fired. */
		expected.start = true;
		expected.tripped = true;
		ok = remains_at(&client, &expected, 150);
	}
	if (ok)
		ok = write_coil(&client, COIL_START, false);
	if (ok)
	{
		expected.start = false;
		ok = remains_at(&client, &expected, 150);
	}
	if (ok)
		ok = write_coil(&client, COIL_RESET, true);
	if (ok)
		ok = write_coil(&client, COIL_RESET, false);
	if (ok)
	{
		expected.tripped = false;
		ok = remains_at(&client, &expected, 200);
	}
	if (ok)
		ok = write_coil(&client, COIL_START, true);
	if (ok)
		ok = wait_starting(&client, &speed);
	if (ok)
		ok = wait_running(&client);
	if (ok)
		usleep(PRIORITY_CLEAR_DELAY_US);
	if (ok)
		ok = write_coil(&client, COIL_START, false);
	if (ok)
		usleep(PRIORITY_SETTLE_US);
	if (ok)
	{
		observation_t final = { 0 };

		/* At this tick both guards are ready: the after-only trip is due
		 * and the conditional stop is true. The marker proves the
		 * after-only destination won; reversed priority would go idle. */
		ok = observe(&client, &final) &&
		     final.tripped &&
		     !final.running &&
		     !final.start &&
		     final.speed == 0;
	}

	client_close(&client);
	bool stopped = fixture_stop();
	ASSERT_EQ(ok && stopped);
}
