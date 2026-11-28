#include "vtest.h"
#include "tests/tests_utils/test_case.h"
#include "build_config.h"
#include "config/config.h"
#include "config/config_device.h"
#include "modbus/pdu.h"
#include "device/machine.h"
#include "vlib/macros.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Writes `text` to a temp file, loads it, and returns the load result. */
static bool
load_text(const char* text, config_t* config, char* err, u32 err_size)
{
	char		path[]  = "/tmp/mogbus-test-XXXXXX";
	arena_t*	persist = ARENA_ALLOC();	/* intentionally kept: config points into it */
	arena_t*	scratch = ARENA_ALLOC();
	int			fd      = mkstemp(path);
	FILE*		f       = fd >= 0 ? fdopen(fd, "wb") : NULL;
	bool		ok      = false;

	if (!f)
	{
		if (fd >= 0)
			close(fd);
		unlink(path);
		snprintf(err, err_size, "cannot create a temporary config file");
		arena_release(scratch);
		return false;
	}

	fputs(text, f);
	fclose(f);
	ok = config_load(persist, scratch, path, config);
	if (!ok)
		snprintf(err, err_size, "%s", config->error);
	unlink(path);
	arena_release(scratch);
	return ok;
}

#define EXPECT_ERROR(text, fragment)							\
	do {										\
		config_t	c = { 0 };						\
		char		e[1024] = "";						\
		ASSERT_FAILURE(load_text((text), &c, e, sizeof(e)));			\
		ASSERT_EQ(strstr(e, (fragment)) != NULL);				\
	} while (0)

#define DEV " (device (unit-id 1) (holding 0))"
#define MODE_LIST "expected one of: tcp, rtu-pty, rtu-device, rtu-tcp, ascii-pty, ascii-device, ascii-tcp, udp, ascii-udp, rtu-udp"

/* Loads `text`, printing the error and failing the test when it does not load. */
static config_t
load_ok(const char* text)
{
	config_t	c = { 0 };
	char		e[512] = "";
	bool		ok = load_text(text, &c, e, sizeof(e));

	if (!ok)
		fprintf(stderr, "config failed to load: %s\n  got: %s\n", text, e);
	ASSERT_EQ(ok);
	return c;
}

/* One config text and the error fragment it must fail with (NULL: it must load). */
struct cfg_case {
	const char*	text;
	const char*	error;
};

/* Runs the rows; when `mode` is set every '@' in a row's text and error is replaced by it. */
static void
run_mode_cases(const char* mode, const struct cfg_case* cases, u32 count)
{
	for (u32 i = 0; i < count; i++)
	{
		config_t	c = { 0 };
		char		text[2048];
		char		error[256];
		char		e[512] = "";
		const char*	row[2] = { cases[i].text, cases[i].error };
		char*		dst[2] = { text, error };
		bool		ok;
		bool		pass;

		for (u32 k = 0; k < 2; k++)
		{
			u32	n = 0;

			for (const char* t = row[k]; t && *t && n + 1 < (k ? 256u : 2048u); t++)
			{
				if (*t != '@' || !mode)
					dst[k][n++] = *t;
				else
					for (const char* m = mode; *m; m++)
						dst[k][n++] = *m;
			}
			if (row[k])
				dst[k][n] = '\0';
		}
		ok = load_text(text, &c, e, sizeof(e));
		pass = cases[i].error ? !ok && strstr(e, error) != NULL : ok;
		if (!pass)
			fprintf(stderr, "config case failed (expected %s): %s\n  got: %s\n",
				cases[i].error ? error : "to load", text, e);
		ASSERT_EQ(pass);
	}
}

static void
run_cases(const struct cfg_case* cases, u32 count)
{
	run_mode_cases(NULL, cases, count);
}

/* Builds head + count copies of item (each '@' becomes the copy's index) + tail into buf. */
static const char*
gen_text(char* buf, u32 size, const char* head, const char* item, u32 count, const char* tail)
{
	u32	n = (u32)snprintf(buf, size, "%s", head);

	for (u32 i = 0; i < count; i++)
	{
		for (const char* t = item; *t && n + 16 < size; t++)
		{
			if (*t == '@')
				n += (u32)snprintf(buf + n, size - n, "%u", i);
			else
				buf[n++] = *t;
		}
	}
	snprintf(buf + n, size - n, "%s", tail);
	return buf;
}

static const struct {
	const char*	name;
	server_mode_t	mode;
	const char*	path_option;
} SERVER_MODES[] = {
	{ "tcp",          SERVER_MODE_TCP,          "" },
	{ "rtu-pty",      SERVER_MODE_RTU_PTY,      "" },
	{ "rtu-device",   SERVER_MODE_RTU_DEVICE,   " (path \"/dev/ttyS0\")" },
	{ "rtu-tcp",      SERVER_MODE_RTU_TCP,      "" },
	{ "ascii-pty",    SERVER_MODE_ASCII_PTY,    "" },
	{ "ascii-device", SERVER_MODE_ASCII_DEVICE, " (path \"/dev/ttyS0\")" },
	{ "ascii-tcp",    SERVER_MODE_ASCII_TCP,    "" },
};

TEST(ConfigLoadsServerAndDevice)
{
	config_t	config = { 0 };
	char		err[256] = "";
	u16		out[2] = { 0, 0 };

	ASSERT_SUCCESS(load_text(
		"; a comment\n"
		"(server (mode tcp) (port 1600) (bind \"127.0.0.1\") (seed 5))\n"
		"(device (name \"Test\") (unit-id 4)\n"
		"  (holding 0 (static 42))\n"
		"  (input 0 (type float32) (static 1.0))\n"
		"  (coil 3 (static on)))\n", &config, err, sizeof(err)));

	ASSERT_EQ(config.servers[0].port == 1600 && config.servers[0].seed == 5);
	ASSERT_EQ(strcmp(config.servers[0].bind_address, "127.0.0.1") == 0);
	ASSERT_EQ(config.device_count == 1);
	ASSERT_EQ(config.devices[0]->unit_id == 4 && strcmp(config.devices[0]->name, "Test") == 0);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 1, out));
	ASSERT_EQ(out[0] == 42);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_INPUT, 0, 2, out));
	ASSERT_EQ(out[0] == 0x3F80 && out[1] == 0);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_COIL, 3, 1, out));
	ASSERT_EQ(out[0] == 1);
}

TEST(ConfigCountExpandsConsecutiveAddresses)
{
	config_t	config = { 0 };
	u16		out[16];

	config = load_ok("(device (unit-id 1) (coil 0 (count 16) (static on)) (holding 10 (type u32) (count 2) (static 7)))");
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_COIL, 0, 16, out));
	ASSERT_EQ(out[15] == 1);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 10, 4, out));
	ASSERT_EQ(out[1] == 7 && out[3] == 7);
	ASSERT_FAILURE(device_read(config.devices[0], SPACE_HOLDING, 14, 1, out));
}

TEST(ConfigPointNoteIsKeptAndSharedByCount)
{
	config_t	config = { 0 };
	device_t*	dev;

	config = load_ok("(device (unit-id 1) (note \"device\") (holding 0 (count 3) (static 1) (note \"Flow\")) (holding 10 (static 1)))");
	dev = config.devices[0];
	ASSERT_EQ(dev->point_count == 4);
	ASSERT_EQ(strcmp(dev->points[0].note, "Flow") == 0 && dev->points[1].note == dev->points[0].note &&
		  dev->points[2].note == dev->points[0].note);
	ASSERT_EQ(dev->points[3].note == NULL);
	EXPECT_ERROR("(device (unit-id 1) (holding 0 (static 1) (note \"01234567890123456789012345678901234567890123456789012345678901230123456789012345678901234567890123456789012345678901234567890123\")))",
		     "note is longer than 127 bytes");
}

TEST(ConfigErrorsAreReadable)
{
	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (holding 0 (static 1))", "syntax error" },
		{ "(device (unit-id 1) (holding 0 (statik 1)))", "unknown option 'statik'" },
		{ "(device (unit-id 1) (holding 70000))", "between 0 and 65535" },
		{ "(device (unit-id 1) (holding 0 (static \"x\")))", "expected a number" },
		{ "(device (unit-id 1) (coil 0 (ramp (from 0) (to 1))))", "can only be (static on/off)" },
		{ "(device (unit-id 1) (holding 0 (ramp (from 0))))", "missing required option (to" },
		{ "(device (unit-id 1) (holding 0) (holding 0))", "already used" },
		{ "(device (unit-id 1) (fault (kind drop)) (fault (kind drop)))", "only one is allowed" },
		{ "(device (unit-id 1) (holding 0) (frobnicate 1))", "unknown item 'frobnicate'" },
		{ "(server (port 1)) (server (port 2))" DEV, "server 1 has no (id" },
		{ "(device (unit-id 1) (holding 0 (ramp (from 0) (to 1e30))))", "too large for the step" },
		{ "(device (unit-id 1) (holding 0 (ramp (from 0) (to 9223372036854775808.0) (step 1) (mode bounce))))",
		  "too large for the step" },
		{ "(device (unit-id 1) (holding 0 (ramp (from 0) (to 9223372036854777856.0) (step 1) (mode bounce))))",
		  "too large for the step" },
		{ "(server (port 1))", "no (device" },
		{ "(device (unit-id 1)) (device (unit-id 1))", "both use unit-id 1" },
		{ "(oops)", "unknown top-level form 'oops'" },
		{ "(device (unit-id 1) (fault (kind exception)) (holding 0))", "missing required option (code ...)" },
		{ "(device (unit-id 1) (fault (kind exception) (code 9)) (holding 0))", "code 9 is reserved" },
		{ "(device (unit-id 1) (fault (kind exception) (code 12)) (holding 0))", "must be between 1 and 11" },
		{ "(device (unit-id 1) (fault (kind exception) (code 0)) (holding 0))", "must be between 1 and 11" },
		{ "(device (unit-id 1) (fault (kind exception) (code \"4\")) (holding 0))",
		  "expected a whole number" },
		{ "(device (unit-id 1) (fault (kind drop) (code 4)) (holding 0))",
		  "code is only valid for kind exception" },
		{ "(device (unit-id 1) (fault (kind corrupt) (code 4)) (holding 0))",
		  "code is only valid for kind exception" },
		{ "(device (unit-id 1) (fault (kind drop) (delay-ms 100)) (holding 0))",
		  "delay-ms is only valid for kind delay" },
		{ "(device (unit-id 1) (fault (kind exception) (code 4) (delay-ms 100)) (holding 0))",
		  "delay-ms is only valid for kind delay" },
		{ "(device (unit-id 1) (fault (kind corrupt) (delay-ms 100)) (holding 0))",
		  "delay-ms is only valid for kind delay" },
		{ "(device (unit-id 1) (fault (kind exception) (code 4) (code 5)) (holding 0))",
		  "given more than once" },
		{ "(device (unit-id 1) (fault (kind disconnect) (delay-ms 100)) (holding 0))",
		  "delay-ms is only valid for kind delay" },
		{ "(device (unit-id 1) (fault (kind disconnect) (code 4)) (holding 0))",
		  "code is only valid for kind exception" },
		{ "(device (unit-id 1) (fault (kind disconnect) (percent 101)) (holding 0))", "between 0 and 100" },
		{ "(device (unit-id 1) (input 0 (sine (offset 0) (amplitude 1))))",
		  "missing required option (period-ms" },
		{ "(device (unit-id 1) (input 0 (sine (offset 0) (period-ms 100))))",
		  "missing required option (amplitude" },
		{ "(device (unit-id 1) (input 0 (sine (offset 0) (amplitude 1) (period-ms 0))))", "period-ms" },
		{ "(device (unit-id 1) (input 0 (sine (offset 0) (amplitude 1) (period-ms 86400001))))",
		  "period-ms" },
		{ "(device (unit-id 1) (input 0 (sine (offset 0) (amplitude 1) (period-ms 5) (speed 2))))",
		  "unknown option 'speed'" },
		{ "(device (unit-id 1) (input 0 (lag (target 1))))", "missing required option (tau-ms" },
		{ "(device (unit-id 1) (input 0 (lag (tau-ms 100))))", "missing required option (target" },
		{ "(device (unit-id 1) (input 0 (lag (target 1) (tau-ms 0))))", "tau-ms" },
		{ "(device (unit-id 1) (input 0 (lag (target \"x\") (tau-ms 10))))", "expected a number or a point" },
		{ "(device (unit-id 1) (input 0 (lag (target (register 1)) (tau-ms 10))))",
		  "expected a number or a point" },
		{ "(device (unit-id 1) (input 0 (lag (target (holding 9)) (tau-ms 10))))",
		  "lag target holding 9 is not the first address of a configured point" },
		{ "(device (unit-id 1) (holding 0 (type u32)) (input 0 (lag (target (holding 1)) (tau-ms 10))))",
		  "is not the first address of a configured point" },
		{ "(device (unit-id 1) (input 0 (lag (target (input 0)) (tau-ms 10))))", "cannot follow itself" },
		{ "(device (unit-id 1) (coil 0 (sine (offset 0) (amplitude 1) (period-ms 10))))",
		  "can only be (static on/off)" },
		{ "(device (unit-id 1) (discrete 0 (lag (target 1) (tau-ms 10))))", "can only be (static on/off)" },
		{ "(device (unit-id 1) (coil 0 (static on) (noise (amplitude 1))))", "cannot have (noise" },
		{ "(device (unit-id 1) (input 0 (noise (amplitude 1))))", "needs a (static" },
		{ "(device (unit-id 1) (input 0 (static 1) (noise (amplitude 1) (distribution cauchy))))",
		  "distribution" },
		{ "(device (unit-id 1) (input 0 (static 1) (noise (every-ms 10))))",
		  "missing required option (amplitude" },
		{ "(device (unit-id 1) (input 0 (static 1) (noise (amplitude -1))))", "0 or more" },
		{ "(device (unit-id 1) (input 0 (static 1) (noise (amplitude 1) (every-ms 0))))", "every-ms" },
		{ "(device (unit-id 1) (input 0 (static 1) (sine (offset 0) (amplitude 1) (period-ms 10))))",
		  "only one of" },
		{ "(device (unit-id 1) (input 0 (sine (offset 0) (amplitude 1) (period-ms 10)) (lag (target 1) (tau-ms 10))))",
		  "only one of" },
		{ "(device (unit-id 1) (input 0 (ramp (from 0) (to 1)) (lag (target 1) (tau-ms 10))))",
		  "only one of" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation)))", "needs at least one of" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (after-ms 100))))", "needs at least one of" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (drift-per-hour 0))))",
		  "needs at least one of" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (noise-growth-per-hour -1))))",
		  "noise-growth-per-hour must be 0 or more" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (fail-after-ms -1))))", "fail-after-ms" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (fail-after-ms 4294967296))))",
		  "fail-after-ms" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (stuck-after-ms -5))))", "stuck-after-ms" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (after-ms 4294967296) (drift-per-hour 1))))",
		  "after-ms" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (fail-after-ms 5) (speed 2))))",
		  "unknown option 'speed'" },
		{ "(device (unit-id 1) (input 0 (degradation (fail-after-ms 5))))", "needs a (static" },
		{ "(device (unit-id 1) (coil 0 (static on) (degradation (drift-per-hour 1))))",
		  "only support stuck-after-ms and fail-after-ms" },
		{ "(device (unit-id 1) (discrete 0 (static on) (degradation (noise-growth-per-hour 1))))",
		  "only support stuck-after-ms and fail-after-ms" },
		{ "(device (unit-id 1) (input 0 (static 1) (degradation (drift-per-hour \"x\"))))",
		  "drift-per-hour" },
		{ "(device (unit-id 1) (holding 0 (static 1) (degradation (fail-after-ms 5)))"
		    " (machine m (initial idle) (state idle (on-enter (set! (holding 0) 3)))))",
		  "machine cannot drive a ramp or modeled point" },
		{ "(device (unit-id 1) (holding 0 (ramp (from 0) (to 4611686018427387904.0) (step 1) (mode bounce))))",
		  NULL },
		{ "(device (unit-id 1) (holding 0 (ramp (from 0) (to 9223372036854775808.0) (step 1) (mode bounce))))",
		  "ramp range is too large for the step" },
		{ "(device (unit-id 1) (holding 0 (ramp (from 0) (to 9223372036854777856.0) (step 1) (mode bounce))))",
		  "ramp range is too large for the step" },
		{ "(device (unit-id 1) (coil 0 (count 8193)))", "8192-point limit" },
		{ "(device (unit-id 1) (coil 0 (count 8192)) (coil 8192 (static on)))", "8192-point limit" },
		{ "(device (name \"a\") (unit-id 1) (any-unit on) (holding 0))"
		    "(device (name \"b\") (unit-id 2) (any-unit on) (holding 0))",
		  "devices 'a' and 'b' both set any-unit" },
		{ "(device (unit-id 1) (any-unit maybe) (holding 0))", "any-unit" },
		{ "(server (mode rtu-tcp)) (device (unit-id 0) (any-unit on) (holding 0))",
		  "not valid for RTU/ASCII" },
		{ "(server (mode ascii-tcp)) (device (unit-id 250) (any-unit on) (holding 0))",
		  "not valid for RTU/ASCII" },
		{ "(device (unit-id 1) (any-unit on) (holding 0))" DEV, "both use unit-id 1" },
		{ "(device (unit-id 1) (fault (kind drop) (mode pdu)) (holding 0))",
		  "mode is only valid for kind corrupt" },
		{ "(device (unit-id 1) (fault (kind restart) (mode register)) (holding 0))",
		  "mode is only valid for kind corrupt" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigDefaults)
{
	config_t	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].port == SERVER_DEFAULT_PORT);
	ASSERT_EQ(config.servers[0].idle_timeout_ms == SERVER_DEFAULT_IDLE_TIMEOUT_MS);
	ASSERT_EQ(config.servers[0].frame_gap_ms == 0);
	ASSERT_EQ(config.servers[0].baud == 0);
	ASSERT_EQ(config.servers[0].strict_t15 == false);
	ASSERT_EQ(config.servers[0].broadcast_execute == false);
	ASSERT_EQ(config.servers[0].data_bits == 8);
	ASSERT_EQ(config.servers[0].parity == PARITY_NONE);
	ASSERT_EQ(config.servers[0].stop_bits == 1);
	ASSERT_EQ(config.devices[0]->unit_id == 1);

	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].mode == SERVER_MODE_TCP);
	ASSERT_EQ(config.servers[0].path[0] == '\0');

	config = load_ok("(server (mode tcp))" DEV);
	ASSERT_EQ(config.servers[0].port == SERVER_DEFAULT_PORT);
	ASSERT_EQ(config.servers[0].tls_enabled == false);
}

TEST(ConfigDeviceRequiresUnitId)
{
	config_t	config = { 0 };

	static const struct cfg_case cases[] = {
		{ "(device (name \"pump\") (holding 0))", "device 'pump': missing (unit-id N)" },
		{ "(device)", "missing (unit-id N)" },
		{ "(device (unit-id 1) (holding 0)) (device (holding 1))", "missing (unit-id N)" },
		{ "(device (unit-id 1) (unit-id 2) (holding 0))", "unit-id is given more than once" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
	config = load_ok("(device (unit-id 0) (holding 0))");
	ASSERT_EQ(config.devices[0]->unit_id == 0);
}

TEST(ConfigAnyUnitDefaultsOffAllowsOneDevice)
{
	config_t	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(!config.devices[0]->any_unit);
	config = load_ok("(device (unit-id 1) (any-unit off) (holding 0))");
	ASSERT_EQ(!config.devices[0]->any_unit);
	config = load_ok("(device (unit-id 1) (holding 0)) (device (unit-id 2) (any-unit on) (holding 0))");
	ASSERT_EQ(!config.devices[0]->any_unit && config.devices[1]->any_unit);
	ASSERT_EQ(config.devices[1]->unit_id == 2);
	config = load_ok("(device (unit-id 1) (any-unit on) (holding 0)) (device (unit-id 2) (any-unit off) (holding 0))");
}

TEST(ConfigFaultIsParsed)
{
	config_t	config = load_ok("(device (unit-id 1) (fault (kind delay) (delay-ms 300) (percent 40)) (holding 0))");
	ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_DELAY));
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_DELAY].delay_ms == 300);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_DELAY].percent == 40);
}

TEST(ConfigFaultExceptionAndCorruptAreParsed)
{
	config_t	config = load_ok("(device (unit-id 1) (fault (kind exception) (code 4)) (holding 0))");
	ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_EXCEPTION));
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_EXCEPTION].exception_code == 4);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_EXCEPTION].percent == 100);

	config = load_ok("(device (unit-id 1) (fault (kind corrupt) (percent 25)) (holding 0))");
	ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_CORRUPT));
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_CORRUPT].exception_code == 0);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_CORRUPT].percent == 25);
}

TEST(ConfigFaultAcceptsEveryStandardExceptionCode)
{
	static const u8 codes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 11 };
	config_t	config = { 0 };
	char		text[128];

	for (u32 i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
	{
		snprintf(text, sizeof(text), "(device (unit-id 1) (fault (kind exception) (code %u)) (holding 0))",
			 codes[i]);
		config = load_ok(text);
		ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_EXCEPTION].exception_code == codes[i]);
	}
}

TEST(ConfigAcceptsMultipleDifferentFaultKinds)
{
	config_t	config = load_ok("(device (unit-id 1) (fault (kind delay) (delay-ms 200) (percent 30))"
		     " (fault (kind drop) (percent 20))"
		     " (fault (kind exception) (code 4) (percent 10))"
		     " (fault (kind corrupt) (percent 5))"
		     " (fault (kind disconnect) (percent 1)) (holding 0))");
	ASSERT_EQ(config.devices[0]->faults.present ==
		  (FAULT_BIT(FAULT_DELAY) | FAULT_BIT(FAULT_DROP) | FAULT_BIT(FAULT_EXCEPTION) |
		   FAULT_BIT(FAULT_CORRUPT) | FAULT_BIT(FAULT_DISCONNECT)));
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_DELAY].delay_ms == 200);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_DELAY].percent == 30);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_DROP].percent == 20);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_EXCEPTION].exception_code == 4);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_EXCEPTION].percent == 10);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_CORRUPT].percent == 5);
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_DISCONNECT].percent == 1);
}

TEST(ConfigRejectsDuplicateFaultKind)
{
	static const char* const texts[] = {
		"(device (unit-id 1) (fault (kind delay) (delay-ms 1)) (fault (kind delay) (delay-ms 2)))",
		"(device (unit-id 1) (fault (kind drop)) (fault (kind drop)))",
		"(device (unit-id 1) (fault (kind exception) (code 2)) (fault (kind exception) (code 3)))",
		"(device (unit-id 1) (fault (kind corrupt)) (fault (kind corrupt)))",
		"(device (unit-id 1) (fault (kind disconnect)) (fault (kind disconnect)))",
	};
	static const char* const kinds[] = { "delay", "drop", "exception", "corrupt", "disconnect" };
	config_t	config = { 0 };
	char		err[256] = "";

	for (u32 i = 0; i < ARRAY_COUNT(texts); i++)
	{
		ASSERT_FAILURE(load_text(texts[i], &config, err, sizeof(err)));
		ASSERT_EQ(strstr(err, "only one is allowed per kind") != NULL);
		ASSERT_EQ(strstr(err, kinds[i]) != NULL);
	}
}

TEST(ConfigDisconnectFaultModeRestrictions)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[256];

	for (u32 i = 0; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		bool tcp_like = SERVER_MODES[i].mode == SERVER_MODE_TCP ||
				SERVER_MODES[i].mode == SERVER_MODE_RTU_TCP ||
				SERVER_MODES[i].mode == SERVER_MODE_ASCII_TCP;

		snprintf(text, sizeof(text),
			 "(server (mode %s)%s) (device (unit-id 1) (fault (kind disconnect) (percent 20)) (holding 0))",
			 SERVER_MODES[i].name, SERVER_MODES[i].path_option);
		if (tcp_like)
		{
			config = load_ok(text);
			ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_DISCONNECT));
			ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_DISCONNECT].percent == 20);
		}
		else
		{
			ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
			ASSERT_EQ(strstr(err, "(fault (kind disconnect)) is not valid") != NULL);
			ASSERT_EQ(strstr(err, SERVER_MODES[i].name) != NULL);
		}
	}
}

TEST(ConfigFaultRestartAndSensorDisconnectAreParsed)
{
	config_t	config = load_ok("(device (unit-id 1) (fault (kind restart) (percent 40)) (holding 0))");
	ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_RESTART));
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_RESTART].percent == 40);

	config = load_ok("(device (unit-id 1) (fault (kind sensor-disconnect) (percent 60)) (holding 0))");
	ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_SENSOR_DISCONNECT));
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_SENSOR_DISCONNECT].percent == 60);
}

TEST(ConfigFaultCorruptModesAreParsed)
{
	static const struct {
		const char*		text;
		fault_corrupt_mode_t	mode;
	} cases[] = {
		{ "(device (unit-id 1) (fault (kind corrupt) (percent 10)) (holding 0))", FAULT_CORRUPT_PDU },
		{ "(device (unit-id 1) (fault (kind corrupt) (mode pdu) (percent 10)) (holding 0))",
		  FAULT_CORRUPT_PDU },
		{ "(device (unit-id 1) (fault (kind corrupt) (mode register) (percent 10)) (holding 0))",
		  FAULT_CORRUPT_REGISTER },
	};
	config_t	config = { 0 };

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		config = load_ok(cases[i].text);
		ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_CORRUPT));
		ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_CORRUPT].corrupt_mode ==
			  cases[i].mode);
	}

	config = load_ok("(server (mode rtu-tcp))"
		     " (device (unit-id 1) (fault (kind corrupt) (mode checksum) (percent 10)) (holding 0))");
	ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_CORRUPT].corrupt_mode ==
		  FAULT_CORRUPT_CHECKSUM);
}

TEST(ConfigCorruptChecksumModeRestrictions)
{
	config_t	config = { 0 };
	char		text[256];

	for (u32 i = 0; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		snprintf(text, sizeof(text),
			 "(server (mode %s)%s) (device (unit-id 1) (fault (kind corrupt) (mode checksum)) (holding 0))",
			 SERVER_MODES[i].name, SERVER_MODES[i].path_option);
		if (SERVER_MODES[i].mode == SERVER_MODE_TCP)
		{
			EXPECT_ERROR(text, "checksum is not valid in tcp mode");
		}
		else
		{
			config = load_ok(text);
			ASSERT_EQ(config.devices[0]->faults.by_kind[FAULT_CORRUPT].corrupt_mode ==
				  FAULT_CORRUPT_CHECKSUM);
		}
	}
}

TEST(ConfigRejectsChecksumOverlayInTcpEvenWhenInactive)
{
	config_t	config = { 0 };

	EXPECT_ERROR(
		"(server (mode tcp)) (device (unit-id 1) (holding 0)"
		" (machine m (initial idle)"
		"  (state idle (fault (kind corrupt) (mode checksum)))))",
		"checksum is not valid in tcp mode");

	config = load_ok("(server (mode rtu-tcp)) (device (unit-id 1) (holding 0)"
		     " (machine m (initial idle)"
		     "  (state idle (fault (kind corrupt) (mode checksum)))))");
	{
		const fault_set_t* overlay =
			&config.devices[0]->machines->defs[0].states[0].faults;

		ASSERT_EQ(overlay->by_kind[FAULT_CORRUPT].corrupt_mode ==
			  FAULT_CORRUPT_CHECKSUM);
	}
}

TEST(ConfigFaultScheduleIsParsed)
{
	config_t	config = { 0 };
	fault_t*	fault  = NULL;

	config = load_ok("(device (unit-id 1) (fault (kind drop)"
		     " (schedule (step (after-ms 100) (count 2))"
		     "           (step (after-ms 50) (count 3)))) (holding 0))");
	fault = &config.devices[0]->faults.by_kind[FAULT_DROP];
	ASSERT_EQ(config.devices[0]->faults.present == FAULT_BIT(FAULT_DROP));
	ASSERT_EQ(fault->schedule_count == 2 && fault->schedule_id != 0);
	ASSERT_EQ(fault->schedule[0].after_ms == 100 && fault->schedule[0].count == 2);
	ASSERT_EQ(fault->schedule[1].after_ms == 50 && fault->schedule[1].count == 3);
}

TEST(ConfigFaultScheduleRejectsBadShapes)
{
	char	text[1024];

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (fault (kind drop) (schedule)) (holding 0))", "schedule needs at least one" },
		{ "(device (unit-id 1) (fault (kind drop) (schedule (step (count 1)))) (holding 0))",
		  "missing required option (after-ms ...)" },
		{ "(device (unit-id 1) (fault (kind drop) (schedule (step (after-ms 1)))) (holding 0))",
		  "missing required option (count ...)" },
		{ "(device (unit-id 1) (fault (kind drop)"
		    " (schedule (step (after-ms 1) (count 1) (bogus 2)))) (holding 0))",
		  "unknown option 'bogus'" },
		{ "(device (unit-id 1) (fault (kind drop)"
		    " (schedule (step (after-ms 1) (after-ms 2) (count 1)))) (holding 0))",
		  "given more than once" },
		{ "(device (unit-id 1) (fault (kind drop)"
		    " (schedule (step (after-ms -1) (count 1)))) (holding 0))",
		  "between 0 and 86400000" },
		{ "(device (unit-id 1) (fault (kind drop)"
		    " (schedule (step (after-ms 86400001) (count 1)))) (holding 0))",
		  "between 0 and 86400000" },
		{ "(device (unit-id 1) (fault (kind drop)"
		    " (schedule (step (after-ms 1) (count 0)))) (holding 0))",
		  "between 1 and 1000000" },
		{ "(device (unit-id 1) (fault (kind drop)"
		    " (schedule (step (after-ms 1) (count 1000001)))) (holding 0))",
		  "between 1 and 1000000" },
		{ "(device (unit-id 1) (fault (kind drop) (percent 5)"
		    " (schedule (step (after-ms 1) (count 1)))) (holding 0))",
		  "percent cannot be combined with schedule" },
	};
	run_cases(cases, ARRAY_COUNT(cases));

EXPECT_ERROR(gen_text(text, sizeof(text), "(device (unit-id 1) (fault (kind drop) (schedule",
			      " (step (after-ms 1) (count 1))", FAULT_MAX_SCHEDULE_STEPS + 1, ")) (holding 0))"),
		     "schedule has more than 8 steps");
}

TEST(ConfigExceptionStatus)
{
	config_t	config = load_ok("(device (unit-id 1) (exception-status 255) (holding 0))");
	ASSERT_EQ(config.devices[0]->exception_status == 255);

	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.devices[0]->exception_status == 0);

	char		err[256] = "";

	ASSERT_FAILURE(load_text("(device (unit-id 1) (exception-status 256) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "between 0 and 255") != NULL);
}

TEST(ConfigReportServerId)
{
	config_t	config = load_ok("(device (unit-id 1) (server-id 42) (run-indicator 0) (holding 0))");
	ASSERT_EQ(config.devices[0]->server_id == 42);
	ASSERT_EQ(config.devices[0]->run_indicator == 0);

	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.devices[0]->server_id == 0);
	ASSERT_EQ(config.devices[0]->run_indicator == 0xFF);

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (server-id 256) (holding 0))", "between 0 and 255" },
		{ "(device (unit-id 1) (run-indicator 256) (holding 0))", "between 0 and 255" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigServerIdAdditionalData)
{
	config_t	config = load_ok("(device (unit-id 1) (server-id-additional-data \"00fFa510\") (holding 0))");
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == 4);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[0] == 0x00);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[1] == 0xFF);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[2] == 0xA5);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[3] == 0x10);

	config = load_ok("(device (unit-id 1) (server-id-additional-data \"\") (holding 0))");
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == 0);

	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == 0);

	char		hex[2 * (DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX + 1) + 1];
	char		text[sizeof(hex) + 64];

	for (u32 i = 0; i < 2 * DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX; i++)
		hex[i] = (i % 2 == 0) ? 'A' : (char)('0' + (i / 2) % 10);
	hex[2 * DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX] = '\0';
	snprintf(text, sizeof(text), "(device (unit-id 1) (server-id-additional-data \"%s\"))", hex);

	config = load_ok(text);
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[0] == 0xA0);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX - 1] == 0xA8);

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (server-id-additional-data \"abc\"))", "even number of hex digits" },
		{ "(device (unit-id 1) (server-id-additional-data \"00gg\"))", "invalid hex digit" },
		{ "(device (unit-id 1) (server-id-additional-data 1234))", "expected text in double quotes" },
	};
	run_cases(cases, ARRAY_COUNT(cases));

	char		err[256] = "";

	memset(hex, '0', sizeof(hex) - 1);
	hex[sizeof(hex) - 1] = '\0';
	snprintf(text, sizeof(text), "(device (unit-id 1) (server-id-additional-data \"%s\"))", hex);

	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "too many hex digits") != NULL);
	ASSERT_EQ(strstr(err, "249 bytes") != NULL);
}

TEST(ConfigComStatus)
{
	config_t	config = load_ok("(device (unit-id 1) (com-status 65535) (holding 0))");
	ASSERT_EQ(config.devices[0]->com_status == 0xFFFF);

	config = load_ok("(device (unit-id 1) (com-status 0) (holding 0))");
	ASSERT_EQ(config.devices[0]->com_status == 0);

	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.devices[0]->com_status == 0);

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (com-status 1) (holding 0))", "must be 0 (ready) or 65535 (busy)" },
		{ "(device (unit-id 1) (com-status 65534) (holding 0))", "must be 0 (ready) or 65535 (busy)" },
		{ "(device (unit-id 1) (com-status -1) (holding 0))", "between 0 and 65535" },
		{ "(device (unit-id 1) (com-status 65536) (holding 0))", "between 0 and 65535" },
		{ "(device (unit-id 1) (com-status \"busy\") (holding 0))", "whole number" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigComEvents)
{
	config_t	config = load_ok("(device (unit-id 1) (com-event 0) (com-event 255) (com-event 7) (holding 0))");
	ASSERT_EQ(config.devices[0]->com_event_log_count == 3);
	ASSERT_EQ(config.devices[0]->com_events[0] == 0);
	ASSERT_EQ(config.devices[0]->com_events[1] == 255);
	ASSERT_EQ(config.devices[0]->com_events[2] == 7);

	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.devices[0]->com_event_log_count == 0);

	char		text[1024];

	gen_text(text, sizeof(text), "(device (unit-id 1) (holding 0)", " (com-event @)", DEVICE_MAX_COM_EVENTS, ")");
	config = load_ok(text);
	ASSERT_EQ(config.devices[0]->com_event_log_count == DEVICE_MAX_COM_EVENTS);
	for (u32 i = 0; i < DEVICE_MAX_COM_EVENTS; i++)
		ASSERT_EQ(config.devices[0]->com_events[i] == (u8)i);

	gen_text(text, sizeof(text), "(device (unit-id 1) (holding 0)", " (com-event @)", DEVICE_MAX_COM_EVENTS + 1, ")");
	EXPECT_ERROR(text, "too many (com-event ...) entries (max 64)");

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (com-event 256) (holding 0))", "between 0 and 255" },
		{ "(device (unit-id 1) (com-event -1) (holding 0))", "between 0 and 255" },
		{ "(device (unit-id 1) (com-event \"x\") (holding 0))", "whole number" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigDeviceIdentification)
{
	config_t	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_VENDOR_NAME], "mogbus") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_PRODUCT_CODE], "mogbus") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_MAJOR_MINOR_REVISION], "1.0") == 0);
	ASSERT_EQ(config.devices[0]->device_id[DEVICE_ID_VENDOR_URL][0] == '\0');

	config = load_ok("(device (unit-id 1) (vendor-name \"Acme\") (product-code \"P1\") (major-minor-revision \"2.3\")"
	     "        (vendor-url \"https://acme.invalid\") (product-name \"Sim\")"
	     "        (model-name \"M\") (user-application-name \"App\") (holding 0))");
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_VENDOR_NAME], "Acme") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_PRODUCT_CODE], "P1") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_MAJOR_MINOR_REVISION], "2.3") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_VENDOR_URL], "https://acme.invalid") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_PRODUCT_NAME], "Sim") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_MODEL_NAME], "M") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_USER_APPLICATION_NAME], "App") == 0);

	char		err[512] = "";
	char		text[512];
	char		longv[246];

	memset(longv, 'x', 245);
	longv[245] = '\0';
	snprintf(text, sizeof(text), "(device (unit-id 1) (vendor-name \"%s\") (holding 0))", longv);
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "vendor-name is too long") != NULL);
	ASSERT_EQ(strstr(err, "FC 43 / 0x2B MEI 14") != NULL);

	char		maxv[245];

	memset(maxv, 'V', 244);
	maxv[244] = '\0';
	snprintf(text, sizeof(text), "(device (unit-id 1) (vendor-name \"%s\") (holding 0))", maxv);
	config = load_ok(text);
	ASSERT_EQ(strlen(config.devices[0]->device_id[DEVICE_ID_VENDOR_NAME]) == 244);
}

TEST(ConfigDeviceIdObject)
{
	config_t	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.devices[0]->extended_id_count == 0);

	config = load_ok("(device (unit-id 1) (device-id-object 200 \"last\") (device-id-object 128 \"first\")"
	     "        (device-id-object 150 \"middle\") (holding 0))");
	ASSERT_EQ(config.devices[0]->extended_id_count == 3);
	ASSERT_EQ(config.devices[0]->extended_id[0].id == 128);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[0].value, "first") == 0);
	ASSERT_EQ(config.devices[0]->extended_id[1].id == 150);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[1].value, "middle") == 0);
	ASSERT_EQ(config.devices[0]->extended_id[2].id == 200);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[2].value, "last") == 0);

	char		text[2048];
	u32		off = 0;
	char		buf[80];

	off += (u32)snprintf(text + off, sizeof(text) - off, "(device (unit-id 1)");
	for (u32 i = 0; i < DEVICE_MAX_EXTENDED_IDS; i++)
		off += (u32)snprintf(text + off, sizeof(text) - off,
				    " (device-id-object %u \"v%u\")", 128 + i, i);
	off += (u32)snprintf(text + off, sizeof(text) - off, " (holding 0))");
	config = load_ok(text);
	ASSERT_EQ(config.devices[0]->extended_id_count == DEVICE_MAX_EXTENDED_IDS);
	ASSERT_EQ(config.devices[0]->extended_id[0].id == 128);
	ASSERT_EQ(config.devices[0]->extended_id[DEVICE_MAX_EXTENDED_IDS - 1].id == 159);
	snprintf(buf, sizeof(buf), "v%d", DEVICE_MAX_EXTENDED_IDS - 1);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[DEVICE_MAX_EXTENDED_IDS - 1].value, buf) == 0);

	config = (config_t){ 0 };
	off = 0;
	off += (u32)snprintf(text + off, sizeof(text) - off, "(device (unit-id 1)");
	for (u32 i = 0; i <= DEVICE_MAX_EXTENDED_IDS; i++)
		off += (u32)snprintf(text + off, sizeof(text) - off,
				    " (device-id-object %u \"v%u\")", 128 + i, i);
	off += (u32)snprintf(text + off, sizeof(text) - off, " (holding 0))");
	EXPECT_ERROR(text, "too many device-id-object entries (max 32)");

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (device-id-object 127 \"x\") (holding 0))",
		  "device-id-object ID must be between 128 and 255" },
		{ "(device (unit-id 1) (device-id-object 256 \"x\") (holding 0))",
		  "device-id-object ID must be between 128 and 255" },
		{ "(device (unit-id 1) (device-id-object 200 \"a\") (device-id-object 200 \"b\") (holding 0))",
		  "device-id-object 200 is configured more than once" },
		{ "(device (unit-id 1) (device-id-object 128 \"\") (holding 0))",
		  "device-id-object text must not be empty" },
	};
	run_cases(cases, ARRAY_COUNT(cases));

	char		longv[246];

	memset(longv, 'x', 245);
	longv[245] = '\0';
	snprintf(text, sizeof(text), "(device (unit-id 1) (device-id-object 128 \"%s\") (holding 0))", longv);
	EXPECT_ERROR(text, "device-id-object text is too long");
}

TEST(ConfigMissingFileIsAnError)
{
	config_t	config = { 0 };
	arena_t*	persist = ARENA_ALLOC();
	arena_t*	scratch = ARENA_ALLOC();

	ASSERT_FAILURE(config_load(persist, scratch, "/nonexistent/file.scm", &config));
	ASSERT_EQ(strstr(config.error, "cannot read") != NULL);
	arena_release(persist);
	arena_release(scratch);
}

TEST(ConfigBoolWords)
{
	config_t	config = { 0 };
	u16		out = 9;

	config = load_ok("(device (unit-id 1) (coil 0 (static yes)) (coil 1 (static #f)) (coil 2 (static 1)))");
	device_read(config.devices[0], SPACE_COIL, 0, 1, &out);
	ASSERT_EQ(out == 1);
	device_read(config.devices[0], SPACE_COIL, 1, 1, &out);
	ASSERT_EQ(out == 0);
	device_read(config.devices[0], SPACE_COIL, 2, 1, &out);
	ASSERT_EQ(out == 1);
}

TEST(ConfigFileRecords)
{
	config_t	config = { 0 };
	u16		out[3] = { 0, 0, 0 };

	config = load_ok("(device (unit-id 1)"
	     "  (file 1 (record 0 4660 22136))"
	     "  (file 4 (record 10 1 2 3)))");
	ASSERT_SUCCESS(device_read_file_records(config.devices[0], 1, 0, 2, out));
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678);
	ASSERT_SUCCESS(device_read_file_records(config.devices[0], 4, 10, 3, out));
	ASSERT_EQ(out[0] == 1 && out[1] == 2 && out[2] == 3);

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (file 1))", "at least one (record" },
		{ "(device (unit-id 1) (file 1 (record 0)))", "(record REC_NO VALUE" },
		{ "(device (unit-id 1) (file 1 (record 10000 1)))", "between 0 and 9999" },
		{ "(device (unit-id 1) (file 0 (record 0 1)))", "between 1 and 65535" },
		{ "(device (unit-id 1) (file 65536 (record 0 1)))", "between 1 and 65535" },
		{ "(device (unit-id 1) (file 1 (record 0 65536)))", "between 0 and 65535" },
		{ "(device (unit-id 1) (file 1 (record 0 1 2) (record 1 3)))", "overlaps another record" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigFifoQueues)
{
	config_t	config = { 0 };
	u16		out[3] = { 0, 0, 0 };
	u16		count  = 0;

	config = load_ok("(device (unit-id 1)"
	     "  (fifo 0 (values 4660 22136))"
	     "  (fifo 1000 (values)))");
	ASSERT_SUCCESS(device_read_fifo(config.devices[0], 0, &count, out));
	ASSERT_EQ(count == 2);
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678);
	ASSERT_SUCCESS(device_read_fifo(config.devices[0], 1000, &count, out));
	ASSERT_EQ(count == 0);

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (fifo))", "expected (fifo ADDRESS (values" },
		{ "(device (unit-id 1) (fifo 0 1))", "expected (values" },
		{ "(device (unit-id 1) (fifo 65536 (values)))", "between 0 and 65535" },
		{ "(device (unit-id 1) (fifo 0 (vals)))", "expected (values" },
		{ "(device (unit-id 1) (fifo 0 (values 65536)))", "between 0 and 65535" },
		{ "(device (unit-id 1) (fifo 0 (values \"x\")))", "whole numbers" },
		{ "(device (unit-id 1) (fifo 0 (values 1)) (fifo 0 (values 2)))", "already defined" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigCanopenObjects)
{
	config_t			config = { 0 };
	const device_canopen_object_t*	obj = NULL;
	u8				out[4] = { 0, 0, 0, 0 };

	config = load_ok("(device (unit-id 1)"
	     "  (canopen-object 5 #x6041 0 \"00fFa510\")"
	     "  (canopen-object 127 0 255 \"aB\"))");
	ASSERT_EQ(config.devices[0]->canopen_object_count == 2);
	obj = device_find_canopen_object(config.devices[0], 5, 0x6041, 0);
	ASSERT_SUCCESS(obj != NULL);
	ASSERT_EQ(obj->node_id == 5 && obj->index == 0x6041 && obj->subindex == 0);
	ASSERT_EQ(obj->length == 4);
	ASSERT_EQ(obj->data[0] == 0x00 && obj->data[1] == 0xFF && obj->data[2] == 0xA5 && obj->data[3] == 0x10);
	ASSERT_SUCCESS(device_canopen_read(config.devices[0], 5, 0x6041, 0, 1, 2, out));
	ASSERT_EQ(out[0] == 0xFF && out[1] == 0xA5);

	obj = device_find_canopen_object(config.devices[0], 127, 0, 255);
	ASSERT_SUCCESS(obj != NULL);
	ASSERT_EQ(obj->length == 1 && obj->data[0] == 0xAB);
	ASSERT_SUCCESS(device_find_canopen_object(config.devices[0], 5, 0x6041, 1) == NULL);

	char		hex[2 * (DEVICE_MAX_CANOPEN_OBJECT_BYTES + 1) + 1];
	char		text[4096];
	for (u32 i = 0; i < 2 * DEVICE_MAX_CANOPEN_OBJECT_BYTES; i++)
		hex[i] = (i % 2 == 0) ? 'A' : (char)('0' + (i / 2) % 10);
	hex[2 * DEVICE_MAX_CANOPEN_OBJECT_BYTES] = '\0';
	snprintf(text, sizeof(text), "(device (unit-id 1) (canopen-object 1 0 0 \"%s\"))", hex);

	config = load_ok(text);
	obj = device_find_canopen_object(config.devices[0], 1, 0, 0);
	ASSERT_SUCCESS(obj != NULL);
	ASSERT_EQ(obj->length == DEVICE_MAX_CANOPEN_OBJECT_BYTES);
	ASSERT_EQ(obj->data[0] == 0xA0);
	ASSERT_EQ(obj->data[DEVICE_MAX_CANOPEN_OBJECT_BYTES - 1] == 0xA7);

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (canopen-object 1 0 0))", "expected (canopen-object NODE INDEX" },
		{ "(device (unit-id 1) (canopen-object \"1\" 0 0 \"00\"))", "expected (canopen-object NODE INDEX" },
		{ "(device (unit-id 1) (canopen-object 0 0 0 \"00\"))", "node must be between 1 and 127" },
		{ "(device (unit-id 1) (canopen-object 128 0 0 \"00\"))", "node must be between 1 and 127" },
		{ "(device (unit-id 1) (canopen-object 1 -1 0 \"00\"))", "index must be between 0 and 65535" },
		{ "(device (unit-id 1) (canopen-object 1 65536 0 \"00\"))", "index must be between 0 and 65535" },
		{ "(device (unit-id 1) (canopen-object 1 0 -1 \"00\"))", "subindex must be between 0 and 255" },
		{ "(device (unit-id 1) (canopen-object 1 0 256 \"00\"))", "subindex must be between 0 and 255" },
		{ "(device (unit-id 1) (canopen-object 1 0 0 1234))", "HEXBYTES must be in double quotes" },
		{ "(device (unit-id 1) (canopen-object 1 0 0 \"\"))", "too few hex digits" },
		{ "(device (unit-id 1) (canopen-object 1 0 0 \"abc\"))", "even number of hex digits" },
		{ "(device (unit-id 1) (canopen-object 1 0 0 \"00gg\"))", "invalid hex digit" },
		{ "(device (unit-id 1) (canopen-object 1 0 0 \"00\") (canopen-object 1 0 0 \"11\"))",
		  "already configured" },
	};
	run_cases(cases, ARRAY_COUNT(cases));

	char		err[1024] = "";
	memset(hex, '0', sizeof(hex) - 1);
	hex[sizeof(hex) - 1] = '\0';
	snprintf(text, sizeof(text), "(device (unit-id 1) (canopen-object 1 0 0 \"%s\"))", hex);

	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "too many hex digits") != NULL);
	ASSERT_EQ(strstr(err, "238 bytes") != NULL);

	char		entry[80];

	strcpy(text, "(device (unit-id 1)");
	for (u32 i = 0; i < DEVICE_MAX_CANOPEN_OBJECTS; i++)
	{
		snprintf(entry, sizeof(entry), " (canopen-object %u 0 0 \"00\")", i + 1);
		strcat(text, entry);
	}
	snprintf(entry, sizeof(entry), " (canopen-object %u 0 0 \"00\"))", DEVICE_MAX_CANOPEN_OBJECTS + 1);
	strcat(text, entry);

	EXPECT_ERROR(text, "too many CANopen objects");
}

TEST(ConfigServerBind)
{
	static const struct {
		const char*	text;
		const char*	bind;
	} loads[] = {
		{ "(server (mode tcp) (bind \"127.0.0.1\"))" DEV, "127.0.0.1" },
		{ "(server (mode tcp))" DEV, "127.0.0.1" },
		{ "(server (mode tcp) (bind \"0.0.0.0\"))" DEV, "0.0.0.0" },
		{ "(server (mode tcp) (bind \"::1\"))" DEV, "::1" },
		{ "(server (mode rtu-tcp) (bind \"::\"))" DEV, "::" },
	};
	char	text[320];
	char	bind[SERVER_BIND_MAX + 8];

	for (u32 i = 0; i < ARRAY_COUNT(loads); i++)
	{
		config_t	config = load_ok(loads[i].text);

		if (strcmp(config.servers[0].bind_address, loads[i].bind) != 0)
			fprintf(stderr, "wrong bind for: %s\n", loads[i].text);
		ASSERT_EQ(strcmp(config.servers[0].bind_address, loads[i].bind) == 0);
	}
	EXPECT_ERROR("(server (mode tcp) (bind \"localhost\"))" DEV, "not a valid IPv4 or IPv6 address");

	memset(bind, 'a', SERVER_BIND_MAX);
	bind[SERVER_BIND_MAX] = '\0';
	snprintf(text, sizeof(text), "(server (mode tcp) (bind \"%s\"))" DEV, bind);
	EXPECT_ERROR(text, "too long");
}

/* Server line keys (timing, serial line and broadcast options): accepted values and rejected ones. */
TEST(ConfigServerLineKeys)
{
	enum { IDLE, GAP, BAUD, SEED, BROADCAST, DATA_BITS, PARITY, STOP_BITS };
	static const struct {
		const char*	text;
		u32		key;
		u32		value;
	} loads[] = {
		{ "(server (mode tcp) (idle-timeout-ms 5000))" DEV, IDLE, 5000 },
		{ "(server (mode rtu-tcp) (idle-timeout-ms 0))" DEV, IDLE, 0 },
		{ "(server (mode rtu-pty) (frame-gap-ms 5))" DEV, GAP, 5 },
		{ "(server (mode ascii-pty) (baud 9600))" DEV, BAUD, 9600 },
		{ "(server (mode ascii-pty) (seed 3) (baud 115200))" DEV, BAUD, 115200 },
		{ "(server (mode ascii-pty) (seed 3) (baud 115200))" DEV, SEED, 3 },
		{ "(server (mode rtu-pty) (broadcast-execute on))" DEV, BROADCAST, true },
		{ "(server (mode rtu-tcp) (broadcast-execute off))" DEV, BROADCAST, false },
		{ "(server (mode rtu-device) (path \"/dev/ttyS0\") (broadcast-execute yes))" DEV, BROADCAST, true },
		{ "(server (mode rtu-pty) (broadcast-execute #f))" DEV, BROADCAST, false },
		{ "(server (mode ascii-pty) (data-bits 7))" DEV, DATA_BITS, 7 },
		{ "(server (mode ascii-device) (path \"/dev/ttyS0\") (data-bits 8))" DEV, DATA_BITS, 8 },
		{ "(server (mode rtu-pty) (parity none))" DEV, PARITY, PARITY_NONE },
		{ "(server (mode rtu-pty) (parity even))" DEV, PARITY, PARITY_EVEN },
		{ "(server (mode rtu-pty) (parity odd))" DEV, PARITY, PARITY_ODD },
		{ "(server (mode rtu-pty) (parity mark))" DEV, PARITY, PARITY_MARK },
		{ "(server (mode rtu-pty) (parity space))" DEV, PARITY, PARITY_SPACE },
		{ "(server (mode rtu-pty) (stop-bits 1))" DEV, STOP_BITS, 1 },
		{ "(server (mode rtu-device) (path \"/dev/ttyS0\") (stop-bits 2))" DEV, STOP_BITS, 2 },
	};
	static const struct cfg_case rejects[] = {
		{ "(server (mode tcp) (idle-timeout-ms -1))" DEV, "between 0 and 86400000" },
		{ "(server (mode rtu-tcp) (idle-timeout-ms 86400001))" DEV, "between 0 and 86400000" },
		{ "(server (mode tcp) (idle-timeout-ms nope))" DEV, "whole number" },
		{ "(server (mode rtu-fd))" DEV, "mode" },
		{ "(server (mode ascii-fd))" DEV, "mode" },
		{ "(server (mode rtu-pty) (frame-gap-ms 0))" DEV, "between 1 and 60000" },
		{ "(server (mode rtu-tcp) (frame-gap-ms 60001))" DEV, "between 1 and 60000" },
		{ "(server (mode rtu-pty) (frame-gap-ms nope))" DEV, "whole number" },
		{ "(server (mode rtu-pty) (baud 0))" DEV, "between 1 and 4000000" },
		{ "(server (mode ascii-pty) (baud 4000001))" DEV, "between 1 and 4000000" },
		{ "(server (mode rtu-pty) (baud nope))" DEV, "whole number" },
		{ "(server (mode rtu-tcp) (baud 12345))" DEV, "standard termios rate" },
		{ "(server (mode rtu-pty) (broadcast-execute maybe))" DEV, "expected on or off" },
		{ "(server (mode rtu-tcp) (broadcast-execute 2))" DEV, "expected on or off" },
		{ "(server (mode ascii-pty) (data-bits 0))" DEV, "between 7 and 8" },
		{ "(server (mode ascii-pty) (data-bits 9))" DEV, "between 7 and 8" },
		{ "(server (mode ascii-device) (path \"/dev/ttyS0\") (data-bits nope))" DEV, "whole number" },
		{ "(server (mode rtu-pty) (data-bits 7))" DEV, "data-bits is not valid in rtu-pty mode" },
		{ "(server (mode tcp) (data-bits 8))" DEV, "data-bits is not valid in tcp mode" },
		{ "(server (mode rtu-tcp) (data-bits 7))" DEV, "data-bits is not valid in rtu-tcp mode" },
		{ "(server (mode rtu-pty) (parity nope))" DEV, "expected one of: none, even, odd, mark, space" },
		{ "(server (mode rtu-device) (path \"/dev/ttyS0\") (parity 1))" DEV,
		  "expected one of: none, even, odd, mark, space" },
		{ "(server (mode rtu-pty) (stop-bits 0))" DEV, "between 1 and 2" },
		{ "(server (mode rtu-device) (path \"/dev/ttyS0\") (stop-bits 3))" DEV, "between 1 and 2" },
		{ "(server (mode rtu-pty) (stop-bits nope))" DEV, "whole number" },
	};

	for (u32 i = 0; i < ARRAY_COUNT(loads); i++)
	{
		config_t	config = load_ok(loads[i].text);
		u32		got[] = { [IDLE] = config.servers[0].idle_timeout_ms, [GAP] = config.servers[0].frame_gap_ms,
					  [BAUD] = config.servers[0].baud, [SEED] = (u32)config.servers[0].seed,
					  [BROADCAST] = config.servers[0].broadcast_execute,
					  [DATA_BITS] = config.servers[0].data_bits, [PARITY] = (u32)config.servers[0].parity,
					  [STOP_BITS] = config.servers[0].stop_bits };

		if (got[loads[i].key] != loads[i].value)
			fprintf(stderr, "wrong value for: %s\n", loads[i].text);
		ASSERT_EQ(got[loads[i].key] == loads[i].value);
	}
	run_cases(rejects, ARRAY_COUNT(rejects));
}

TEST(ConfigServerModeAndPath)
{
	static const struct cfg_case cases[] = {
		{ "(server (mode rtu))" DEV, MODE_LIST },
		{ "(server (mode \"tcp\"))" DEV, MODE_LIST },
		{ "(server (mode rtu-device))" DEV, "(path ...) is required" },
		{ "(server (mode ascii-device))" DEV, "(path ...) is required" },
		{ "(server (mode tcp) (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
		{ "(server (mode rtu-pty) (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
		{ "(server (mode rtu-tcp) (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
		{ "(server (mode ascii-pty) (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
		{ "(server (mode ascii-tcp) (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
		{ "(server (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
	};
	config_t	config;
	char		path[SERVER_PATH_MAX + 1];
	char		text[SERVER_PATH_MAX + 128];

	for (u32 i = 0; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		snprintf(text, sizeof(text), "(server (mode %s)%s)" DEV,
			 SERVER_MODES[i].name, SERVER_MODES[i].path_option);
		config = load_ok(text);
		ASSERT_EQ(config.servers[0].mode == SERVER_MODES[i].mode);
		if (*SERVER_MODES[i].path_option)
			ASSERT_EQ(strcmp(config.servers[0].path, "/dev/ttyS0") == 0);
		else
			ASSERT_EQ(config.servers[0].path[0] == '\0');
	}

	config = load_ok("(server (mode rtu-device) (path \"/dev/ttyUSB0\"))" DEV);
	ASSERT_EQ(config.servers[0].mode == SERVER_MODE_RTU_DEVICE);
	ASSERT_EQ(strcmp(config.servers[0].path, "/dev/ttyUSB0") == 0);
	config = load_ok("(server (mode ascii-device) (path \"/dev/ttyUSB1\"))" DEV);
	ASSERT_EQ(config.servers[0].mode == SERVER_MODE_ASCII_DEVICE);
	ASSERT_EQ(strcmp(config.servers[0].path, "/dev/ttyUSB1") == 0);
	run_cases(cases, ARRAY_COUNT(cases));

	memset(path, 'x', SERVER_PATH_MAX - 1);
	path[SERVER_PATH_MAX - 1] = '\0';
	snprintf(text, sizeof(text), "(server (mode rtu-device) (path \"%s\"))" DEV, path);
	config = load_ok(text);
	ASSERT_EQ(strlen(config.servers[0].path) == SERVER_PATH_MAX - 1);

	memset(path, 'x', SERVER_PATH_MAX);
	path[SERVER_PATH_MAX] = '\0';
	snprintf(text, sizeof(text), "(server (mode rtu-device) (path \"%s\"))" DEV, path);
	EXPECT_ERROR(text, "path is too long");
	EXPECT_ERROR(text, "255 characters");
}

#if WITH_TLS
TEST(ConfigTlsFullFormIsParsed)
{
	config_t	config = load_ok("(server (mode tcp)\n"
		     "  (tls (certificate \"/tmp/server.pem\")\n"
		     "       (private-key \"/tmp/server.key\")\n"
		     "       (ca-certificate \"/tmp/ca.pem\")\n"
		     "       (handshake-timeout-ms 250)\n"
		     "       (require-client-certificate off)\n"
		     "       (min-version tls1.3)\n"
		     "       (tls1-2-ciphers \"ECDHE-RSA-AES128-GCM-SHA256\")\n"
		     "       (tls1-3-ciphersuites \"TLS_AES_128_GCM_SHA256\")))\n"
		     "(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].mode == SERVER_MODE_TCP);
	ASSERT_EQ(config.servers[0].port == CONFIG_DEFAULT_TLS_PORT);
	ASSERT_EQ(config.servers[0].tls_enabled);
	ASSERT_EQ(strcmp(config.servers[0].tls_certificate, "/tmp/server.pem") == 0);
	ASSERT_EQ(strcmp(config.servers[0].tls_private_key, "/tmp/server.key") == 0);
	ASSERT_EQ(strcmp(config.servers[0].tls_ca_certificate, "/tmp/ca.pem") == 0);
	ASSERT_EQ(config.servers[0].tls_require_client_certificate == false);
	ASSERT_EQ(config.servers[0].tls_min_version == CONFIG_TLS_VERSION_1_3);
	ASSERT_EQ(config.servers[0].tls_handshake_timeout_ms == 250);
	ASSERT_EQ(strcmp(config.servers[0].tls1_2_ciphers, "ECDHE-RSA-AES128-GCM-SHA256") == 0);
	ASSERT_EQ(strcmp(config.servers[0].tls1_3_ciphersuites, "TLS_AES_128_GCM_SHA256") == 0);
}

TEST(ConfigTlsDefaultsAndExplicitPort)
{
	config_t	config = load_ok("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\")\n"
		     "             (ca-certificate \"ca.pem\")))\n"
		     "(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].port == CONFIG_DEFAULT_TLS_PORT);
	ASSERT_EQ(config.servers[0].tls_enabled);
	ASSERT_EQ(config.servers[0].tls_require_client_certificate == true);
	ASSERT_EQ(config.servers[0].tls_min_version == CONFIG_TLS_VERSION_1_2);
	ASSERT_EQ(config.servers[0].tls_handshake_timeout_ms == SERVER_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS);
	ASSERT_EQ(config.servers[0].tls1_2_ciphers[0] == '\0');
	ASSERT_EQ(config.servers[0].tls1_3_ciphersuites[0] == '\0');

	config = load_ok("(server (mode tcp) (port 1802)\n"
		     "        (tls (certificate \"cert.pem\") (private-key \"key.pem\")\n"
		     "             (require-client-certificate off)))\n"
		     "(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].port == 1802);
	ASSERT_EQ(config.servers[0].tls_ca_certificate[0] == '\0');
	ASSERT_EQ(config.servers[0].tls_require_client_certificate == false);
}

TEST(ConfigTlsOptions)
{
	config_t	config = load_ok("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
	     "             (ca-certificate \"ca.pem\") "
	     "             (handshake-timeout-ms 250)))" DEV);
	ASSERT_EQ(config.servers[0].tls_handshake_timeout_ms == 250);

	config = load_ok("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
	     "             (ca-certificate \"ca.pem\") "
	     "             (handshake-timeout-ms 1)))" DEV);
	ASSERT_EQ(config.servers[0].tls_handshake_timeout_ms == 1);

	config = load_ok("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
	     "             (ca-certificate \"ca.pem\") "
	     "             (handshake-timeout-ms 86400000)))" DEV);
	ASSERT_EQ(config.servers[0].tls_handshake_timeout_ms == 86400000);

	config = load_ok("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(require-client-certificate off) (min-version tls1.2))) "
		     "(device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].tls_min_version == CONFIG_TLS_VERSION_1_2);

	char	path[SERVER_TLS_PATH_MAX + 2];
	char	text[SERVER_TLS_PATH_MAX + 160];

	memset(path, 'x', SERVER_TLS_PATH_MAX);
	path[SERVER_TLS_PATH_MAX] = '\0';
	snprintf(text, sizeof(text),
		 "(server (tls (certificate \"%s\") (private-key \"key.pem\"))) "
		 "(device (unit-id 1) (holding 0))", path);
	EXPECT_ERROR(text, "certificate path is too long");

	static const struct cfg_case cases[] = {
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "         (ca-certificate \"ca.pem\") "
		    "         (handshake-timeout-ms 0)))" DEV,
		  "between 1 and 86400000" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "         (ca-certificate \"ca.pem\") "
		    "         (handshake-timeout-ms 86400001)))" DEV,
		  "between 1 and 86400000" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "         (ca-certificate \"ca.pem\") "
		    "         (handshake-timeout-ms nope)))" DEV,
		  "whole number" },
		{ "(server (handshake-timeout-ms 250) "
		    "        (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "        (ca-certificate \"ca.pem\"))) "
		    "(device (unit-id 1) (holding 0))",
		  "unknown option 'handshake-timeout-ms'" },
		{ "(server (tls (private-key \"key.pem\") (ca-certificate \"ca.pem\"))) "
		    "(device (unit-id 1) (holding 0))",
		  "missing required option (certificate ...)" },
		{ "(server (tls (certificate \"cert.pem\") (ca-certificate \"ca.pem\"))) "
		    "(device (unit-id 1) (holding 0))",
		  "missing required option (private-key ...)" },
		{ "(server (tls (certificate \"\") (private-key \"key.pem\"))) "
		    "(device (unit-id 1) (holding 0))",
		  "certificate path must not be empty" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\"))) "
		    "(device (unit-id 1) (holding 0))",
		  "ca-certificate is required" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(require-client-certificate on)))" DEV,
		  "ca-certificate is required" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(min-version tls1.1)))" DEV,
		  "expected one of: tls1.2, tls1.3" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(min-version \"tls1.3\")))" DEV,
		  "expected one of: tls1.2, tls1.3" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(require-client-certificate maybe)))" DEV,
		  "expected on or off" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(tls1-2-ciphers \"\")))" DEV,
		  "tls1-2-ciphers must not be empty" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(tls1-3-ciphersuites \"\")))" DEV,
		  "tls1-3-ciphersuites must not be empty" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(tls1-2-ciphers 7)))" DEV,
		  "expected text in double quotes" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(tls1-3-ciphersuites 7)))" DEV,
		  "expected text in double quotes" },
		{ "(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		    "(unknown 1)))" DEV,
		  "unknown option 'unknown'" },
		{ "(server (tls (certificate \"cert.pem\") (certificate \"other.pem\") "
		    "(private-key \"key.pem\")))" DEV,
		  "option 'certificate' is given more than once" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}


TEST(ConfigTlsIsRejectedOutsideTcpMode)
{
	char	text[256];

	for (u32 i = 1; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		snprintf(text, sizeof(text), "(server (mode %s)%s (tls))" DEV,
			 SERVER_MODES[i].name, SERVER_MODES[i].path_option);
		EXPECT_ERROR(text, "not valid in");
	}
}
#else
TEST(ConfigTlsRejectedWithoutTlsSupport)
{
	static const struct cfg_case cases[] = {
		{ "(server (mode tcp) (tls (certificate \"cert.pem\") "
		    "(private-key \"key.pem\") (ca-certificate \"ca.pem\"))) "
		    "(device (unit-id 1) (holding 0))",
		  "built without TLS support" },
		{ "(server (mode rtu-pty) (tls))" DEV, "built without TLS support" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}
#endif

TEST(ConfigSerialModesRejectReservedUnitIds)
{
	config_t		config = { 0 };
	char			err[256] = "";
	char			text[256];
	char			fragment[80];
	static const u32	ids[] = { 0, 248, 255 };

	for (u32 i = 1; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		for (u32 j = 0; j < ARRAY_COUNT(ids); j++)
		{
			snprintf(text, sizeof(text), "(server (mode %s)%s) (device (unit-id %u) (holding 0))",
				 SERVER_MODES[i].name, SERVER_MODES[i].path_option, ids[j]);
			ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
			snprintf(fragment, sizeof(fragment), "unit-id %u is not valid for RTU/ASCII", ids[j]);
			ASSERT_EQ(strstr(err, fragment) != NULL);
		}
	}
}

TEST(ConfigSerialModesAcceptUnitIdBoundaries)
{
	config_t		config = { 0 };
	char			text[256];
	static const u32	ids[] = { 1, 247 };

	for (u32 i = 1; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		for (u32 j = 0; j < ARRAY_COUNT(ids); j++)
		{
			snprintf(text, sizeof(text), "(server (mode %s)%s) (device (unit-id %u) (holding 0))",
				 SERVER_MODES[i].name, SERVER_MODES[i].path_option, ids[j]);
			config = load_ok(text);
			ASSERT_EQ(config.devices[0]->unit_id == ids[j]);
		}
	}
}

TEST(ConfigTcpModeAcceptsFullUnitIdRange)
{
	config_t		config = { 0 };
	char			text[192];
	static const u32	ids[] = { 0, 255 };

	for (u32 i = 0; i < ARRAY_COUNT(ids); i++)
	{
		snprintf(text, sizeof(text), "(server (mode tcp)) (device (unit-id %u) (holding 0))", ids[i]);
		config = load_ok(text);
		ASSERT_EQ(config.servers[0].mode == SERVER_MODE_TCP);
		ASSERT_EQ(config.devices[0]->unit_id == ids[i]);
	}
}

TEST(ConfigServerOptionsRejectedPerMode)
{
	/* Presence alone is invalid here: even the default value is rejected. */

	static const struct cfg_case cases[] = {
		{ "(server (mode rtu-tcp) (strict-t15 on))" DEV, "strict-t15 is not valid in rtu-tcp mode" },
		{ "(server (mode rtu-tcp) (strict-t15 off))" DEV, "strict-t15 is not valid in rtu-tcp mode" },
		{ "(server (mode rtu-tcp) (parity none))" DEV, "parity is not valid in rtu-tcp mode" },
		{ "(server (mode rtu-tcp) (parity even))" DEV, "parity is not valid in rtu-tcp mode" },
		{ "(server (mode rtu-tcp) (stop-bits 1))" DEV, "stop-bits is not valid in rtu-tcp mode" },
		{ "(server (mode rtu-tcp) (stop-bits 2))" DEV, "stop-bits is not valid in rtu-tcp mode" },
		{ "(server (mode tcp) (broadcast-execute on))" DEV, "broadcast-execute is not valid in tcp mode" },
		{ "(server (broadcast-execute off))" DEV, "broadcast-execute is not valid in tcp mode" },
		{ "(server (mode tcp) (baud 9600))" DEV, "baud is not valid in tcp mode" },
		{ "(server (mode tcp) (frame-gap-ms 20))" DEV, "frame-gap-ms is not valid in tcp mode" },
		{ "(server (mode tcp) (strict-t15 off))" DEV, "strict-t15 is not valid in tcp mode" },
		{ "(server (mode tcp) (parity none))" DEV, "parity is not valid in tcp mode" },
		{ "(server (mode tcp) (stop-bits 1))" DEV, "stop-bits is not valid in tcp mode" },
		{ "(server (mode tcp) (data-bits 7))" DEV, "data-bits is not valid in tcp mode" },
		{ "(server (mode ascii-pty) (broadcast-execute on))" DEV,
		  "broadcast-execute is not valid in ascii-pty mode (ASCII never executes unit-0 broadcasts)" },
		{ "(server (mode ascii-device) (path \"/dev/ttyS0\") (broadcast-execute off))"
		    " (device (unit-id 1) (holding 0))",
		  "broadcast-execute is not valid in ascii-device mode (ASCII never executes unit-0 broadcasts)" },
		{ "(server (mode ascii-pty) (frame-gap-ms 20))" DEV, "frame-gap-ms is not valid in ascii-pty mode" },
		{ "(server (mode ascii-pty) (strict-t15 on))" DEV, "strict-t15 is not valid in ascii-pty mode" },
		{ "(server (mode ascii-tcp) (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
		{ "(server (mode ascii-tcp) (baud 9600))" DEV, "baud is not valid in ascii-tcp mode" },
		{ "(server (mode ascii-tcp) (frame-gap-ms 20))" DEV, "frame-gap-ms is not valid in ascii-tcp mode" },
		{ "(server (mode ascii-tcp) (strict-t15 on))" DEV, "strict-t15 is not valid in ascii-tcp mode" },
		{ "(server (mode ascii-tcp) (broadcast-execute on))" DEV,
		  "broadcast-execute is not valid in ascii-tcp mode" },
		{ "(server (mode ascii-tcp) (broadcast-execute off))" DEV,
		  "broadcast-execute is not valid in ascii-tcp mode (ASCII never executes unit-0 broadcasts)" },
		{ "(server (mode ascii-tcp) (data-bits 7))" DEV, "data-bits is not valid in ascii-tcp mode" },
		{ "(server (mode ascii-tcp) (parity even))" DEV, "parity is not valid in ascii-tcp mode" },
		{ "(server (mode ascii-tcp) (stop-bits 1))" DEV, "stop-bits is not valid in ascii-tcp mode" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigRtuTcpAcceptsListenAndTimingOptions)
{
	config_t	config = load_ok("(server (mode rtu-tcp) (port 1503) (bind \"127.0.0.1\") (seed 4)"
		     " (idle-timeout-ms 500) (baud 9600) (frame-gap-ms 20) (broadcast-execute on))"
		     " (device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].port == 1503);
	ASSERT_EQ(strcmp(config.servers[0].bind_address, "127.0.0.1") == 0);
	ASSERT_EQ(config.servers[0].seed == 4);
	ASSERT_EQ(config.servers[0].idle_timeout_ms == 500);
	ASSERT_EQ(config.servers[0].baud == 9600);
	ASSERT_EQ(config.servers[0].frame_gap_ms == 20);
	ASSERT_EQ(config.servers[0].broadcast_execute == true);
}

TEST(ConfigSerialModesRejectTcpListenOptions)
{
	char		text[256];
	char		fragment[80];
	static const char* const options[] = {
		"(port 1502)", "(bind \"127.0.0.1\")", "(idle-timeout-ms 100)"
	};

	for (u32 i = 1; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		if (SERVER_MODES[i].mode == SERVER_MODE_RTU_TCP ||
		    SERVER_MODES[i].mode == SERVER_MODE_ASCII_TCP)
			continue;
		for (u32 j = 0; j < ARRAY_COUNT(options); j++)
		{
			snprintf(text, sizeof(text), "(server (mode %s)%s %s)" DEV,
				 SERVER_MODES[i].name, SERVER_MODES[i].path_option, options[j]);
			snprintf(fragment, sizeof(fragment), "is not valid in %s mode", SERVER_MODES[i].name);
			EXPECT_ERROR(text, fragment);
		}
	}
}

TEST(ConfigAsciiModeAcceptsLineSettings)
{
	config_t	config = load_ok("(server (mode ascii-pty) (seed 3) (baud 115200)"
		     " (data-bits 7) (parity even) (stop-bits 1))" DEV);
	ASSERT_EQ(config.servers[0].baud == 115200);
	ASSERT_EQ(config.servers[0].seed == 3);
	ASSERT_EQ(config.servers[0].data_bits == 7);
	ASSERT_EQ(config.servers[0].parity == PARITY_EVEN);
	ASSERT_EQ(config.servers[0].stop_bits == 1);

	config = load_ok("(server (mode ascii-device) (path \"/dev/ttyS0\") (data-bits 8)"
		     " (parity odd) (stop-bits 2))" DEV);
	ASSERT_EQ(config.servers[0].data_bits == 8);
	ASSERT_EQ(config.servers[0].parity == PARITY_ODD);
	ASSERT_EQ(config.servers[0].stop_bits == 2);
}

TEST(ConfigAsciiTcpAcceptsListenOptions)
{
	config_t	config = load_ok("(server (mode ascii-tcp))" DEV);
	ASSERT_EQ(config.servers[0].mode == SERVER_MODE_ASCII_TCP);
	ASSERT_EQ(config.servers[0].port == SERVER_DEFAULT_PORT);
	ASSERT_EQ(strcmp(config.servers[0].bind_address, "127.0.0.1") == 0);

	config = load_ok("(server (mode ascii-tcp) (port 1503) (bind \"::1\") (seed 4) (idle-timeout-ms 500))"
		     " (device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].port == 1503);
	ASSERT_EQ(strcmp(config.servers[0].bind_address, "::1") == 0);
	ASSERT_EQ(config.servers[0].seed == 4);
	ASSERT_EQ(config.servers[0].idle_timeout_ms == 500);
}

TEST(ConfigSerialModesAcceptSerialOptions)
{
	config_t	config = load_ok("(server (mode rtu-pty) (strict-t15 on) (parity even) (stop-bits 2) (baud 9600))"
		     " (device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].strict_t15 == true);
	ASSERT_EQ(config.servers[0].parity == PARITY_EVEN);
	ASSERT_EQ(config.servers[0].stop_bits == 2);
	ASSERT_EQ(config.servers[0].baud == 9600);
}

TEST(ConfigNonFiniteStatic)
{
	config_t	config = { 0 };
	u16		out[2] = { 0, 0 };
	u32		raw = 0;
	f32		real = 0;

	config = load_ok("(device (unit-id 1) (holding 0 (type float32) (static +nan.0))\n"
	     "        (holding 2 (type float32) (static +inf.0))\n"
	     "        (holding 4 (type float32) (static -inf.0)))");
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 2, out));
	raw = ((u32)out[0] << 16) | out[1];
	memcpy(&real, &raw, sizeof(real));
	ASSERT_EQ(isnan(real));
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 2, 2, out));
	ASSERT_EQ(out[0] == 0x7F80 && out[1] == 0x0000);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 4, 2, out));
	ASSERT_EQ(out[0] == 0xFF80 && out[1] == 0x0000);

	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (holding 0 (type u16) (static +nan.0)))", "only float32" },
		{ "(device (unit-id 1) (holding 0 (type u16) (static +inf.0)))", "only float32" },
		{ "(device (unit-id 1) (holding 0 (type s32) (static -inf.0)))", "only float32" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

#define MACHINE_DEVICE(body) "(device (unit-id 1) (holding 0) (machine " body "))"

#define RANDOM_MACHINE_DEVICE \
	"(device (unit-id 1) (holding 0)" \
	"  (machine m (initial idle)" \
	"    (state idle) (state done)" \
	"    (transition idle done (after-ms (random 1 86400000)))))"

/* Returns the initial random after-ms deadline relative to device start time. */
static u64
load_random_deadline(u32 seed, bool server_first, config_t* config, char* err, u32 err_size)
{
	char	text[512];
	char	server[64];
	u64	offset = 0;

	snprintf(server, sizeof(server), "(server (mode tcp) (seed %u))", seed);
	if (server_first)
		snprintf(text, sizeof(text), "%s %s", server, RANDOM_MACHINE_DEVICE);
	else
		snprintf(text, sizeof(text), "%s %s", RANDOM_MACHINE_DEVICE, server);
	ASSERT_SUCCESS(load_text(text, config, err, err_size));
	ASSERT_EQ(config->servers[0].seed == seed);
	ASSERT_SUCCESS(config->device_count == 1 &&
		       config->devices[0]->machines != NULL &&
		       config->devices[0]->machines->count == 1);
	offset = config->devices[0]->machines->deadline[0][0] - config->devices[0]->start_ms;
	ASSERT_EQ(offset >= 1 && offset <= 86400000);
	return offset;
}

TEST(ConfigMachineLoadsCompiledDefinitionAndRuns)
{
	config_t	config = { 0 };
	device_t*	dev = NULL;
	const machine_def_t* def = NULL;
	u16		out[1] = { 0 };

	config = load_ok("(server (seed 123))"
		     " (device (unit-id 1) (holding 0)"
		     "  (machine m (initial idle)"
		     "    (var n 5) (var flag #t)"
		     "    (state idle (on-enter (set! (holding 0) n)))"
		     "    (state done (on-enter (set! (holding 0) 42)))"
		     "    (transition idle done (after-ms 10))))");

	dev = config.devices[0];
	ASSERT_EQ(dev->machines->count == 1 && dev->machines->seed == 123);
	def = &dev->machines->defs[0];
	ASSERT_EQ(strcmp(def->name, "m") == 0 && def->initial_state == 0);
	ASSERT_EQ(def->var_count == 2 && strcmp(def->vars[0].name, "n") == 0);
	ASSERT_EQ(def->vars[0].initial.n == 5 && def->vars[1].initial.b == true);
	ASSERT_EQ(def->state_count == 2 && def->transition_count == 1);
	ASSERT_EQ(def->transitions[0].from == 0 && def->transitions[0].to == 1);
	ASSERT_EQ(def->transitions[0].after_kind == AFTER_LITERAL);
	ASSERT_EQ(def->transitions[0].after_literal_ms == 10);

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, out));
	ASSERT_EQ(out[0] == 5);
	device_tick(dev, dev->start_ms + 20);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, out));
	ASSERT_EQ(out[0] == 42);
}

TEST(ConfigMachineStorageIsLazyPerDevice)
{
	config_t	config = load_ok("(server (seed 1))"
		     " (device (unit-id 1) (name \"plain\") (holding 0))"
		     " (device (unit-id 2) (name \"driven\") (holding 0)"
		     "  (machine m (initial idle)"
		     "    (state idle (on-tick (set! (holding 0) 7)))))");

	ASSERT_EQ(config.device_count == 2);
	ASSERT_EQ(strcmp(config.devices[0]->name, "plain") == 0);
	ASSERT_EQ(config.devices[0]->machines == NULL);
	ASSERT_EQ(strcmp(config.devices[1]->name, "driven") == 0);
	ASSERT_SUCCESS(config.devices[1]->machines != NULL);
	ASSERT_EQ(config.devices[1]->machines->count == 1);
}

TEST(ConfigMachineSeedIgnoresServerFormOrder)
{
	config_t first = { 0 };
	config_t last  = { 0 };
	config_t other = { 0 };
	char	err[256] = "";
	u64	first_offset = 0;
	u64	last_offset  = 0;
	u64	other_offset = 0;

	first_offset = load_random_deadline(5, true, &first, err, sizeof(err));
	last_offset  = load_random_deadline(5, false, &last, err, sizeof(err));
	other_offset = load_random_deadline(6, false, &other, err, sizeof(err));

	ASSERT_EQ(first_offset == last_offset);
	ASSERT_EQ(first_offset != other_offset);
}

TEST(ConfigMachineDeferredErrorKeepsFormTextWithServerLast)
{
	EXPECT_ERROR(
		"(device (unit-id 1) (holding 0)"
		" (machine m (initial idle)"
		"  (state idle (on-enter (when (< (holding 5) 1) (set! (holding 0) 2))))))"
		" (server (seed 5))",
		"(holding 5): machine 'm', state 'idle': point read must name the first address of a configured point");
}

TEST(ConfigMachineResolvesForwardPointsAndNestedGuards)
{
	config_t	config = { 0 };
	u16		out[1] = { 0 };

	config = load_ok("(device (unit-id 1)"
		     "  (machine m (initial idle)"
		     "    (var n 0)"
		     "    (state idle"
		     "      (on-tick (when #t (set! n 3) (when #f (set! n 9))"
		     "                 (set! (holding 0) n)))))"
		     "  (holding 0))");

	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 1, out));
	ASSERT_EQ(out[0] == 0);
	device_tick(config.devices[0], config.devices[0]->start_ms);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 1, out));
	ASSERT_EQ(out[0] == 3); /* the nested false guard did not run */
}

TEST(ConfigMachineRejects)
{
	static const struct cfg_case cases[] = {
		{ MACHINE_DEVICE("m (initial idle) (bogus) (state idle)"), "unknown machine form" },
		{ MACHINE_DEVICE("M (initial idle) (state idle)"), "bad machine name" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (state idle))"
		    " (machine m (initial idle) (state idle)))",
		  "defined more than once" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0) (var n 1) (state idle)"), "duplicate variable name" },
		{ MACHINE_DEVICE("m (initial idle) (state idle) (state idle)"), "duplicate state name" },
		{ MACHINE_DEVICE("m (initial idle) (state idle (on-tick) (on-tick))"), "given more than once" },
		{ MACHINE_DEVICE("m (initial missing) (state idle)"), "unknown initial state" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle missing (after-ms 1))"),
		  "unknown initial or transition state" },
		{ MACHINE_DEVICE("m (initial idle)"), "(machine m (initial idle)): machine has no states" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (bogus))"),
		  "unknown transition trigger" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle)"),
		  "state 'idle', (transition idle): expected (transition FROM TO TRIGGER ...)" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (+ #t 1))))"),
		  "operands must be numbers" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (when (< #t #f) (set! n 1))))"),
		  "operands must be numbers" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (when (= #t 0) (set! n 1))))"),
		  "equality operands must have the same type" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (when (and #t 1) (set! n 1))))"),
		  "logical operands must be booleans" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (when (not 1) (set! n 1))))"),
		  "NOT operand must be a boolean" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (if 1 2 3))))"),
		  "IF condition must be a boolean" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (if #t 1 #f))))"),
		  "IF branches must have the same type" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n #t)))"),
		  "assignment value has the wrong type" },
		{ "(device (unit-id 1) (coil 0) (holding 0)"
		    " (machine m (initial idle)"
		    "  (state idle (on-enter (set! (coil 0) 1)))))",
		  "assignment value has the wrong type" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (when 1 (set! n 2))))"),
		  "condition must be a boolean" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (when (set! n 1) (set! n 2))))"),
		  "statement form used as expression" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-tick (+ n 1)))"),
		  "expression form used as statement" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (foo))))"),
		  "unknown expression form" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (+ 1))))"),
		  "expected exactly two operands" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (now-ms 1))))"),
		  "expected (now-ms) with no operands" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (not))))"),
		  "expected exactly one operand" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (set! n (if #t 1))))"),
		  "expected (if CONDITION THEN ELSE)" },
		{ MACHINE_DEVICE("m (initial idle) (var n 0)"
		    " (state idle (on-enter (when (and #t) (set! n 1))))"),
		  "expected at least two operands" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle)"
		    "  (state idle (on-enter (when (< (holding 5) 1) (set! (holding 0) 2))))))",
		  "(holding 5)" },
		{ "(device (unit-id 1) (holding 0 (type u32))"
		    " (machine m (initial idle)"
		    "  (state idle (on-enter (set! (holding 1) 3)))))",
		  "not the first address of a configured point" },
		{ "(device (unit-id 1)"
		    " (holding 0 (ramp (from 0) (to 10) (every-ms 100)))"
		    " (machine m (initial idle)"
		    "  (state idle (on-enter (set! (holding 0) 3)))))",
		  "(holding 0): machine 'm', state 'idle': machine cannot drive a ramp or modeled point" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine a (initial idle)"
		    "  (state idle (on-enter (set! (holding 0) 1))))"
		    " (machine b (initial idle)"
		    "  (state idle (on-enter (set! (holding 0) 2)))))",
		  "(holding 0): machine 'b', state 'idle': point holding 0 is already driven by another machine" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine a (initial idle)"
		    "  (state idle (on-enter (set! (holding 0) (holding 0))))))"
		    " (device (unit-id 2) (holding 0)"
		    " (machine b (initial running)"
		    "  (state running (on-enter (set! (holding 0) (holding 8))))))",
		  "(holding 8)" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (after-ms 0))"),
		  "(after-ms 0): after-ms must be between 1 and 86400000" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (after-ms 86400001))"),
		  "(after-ms 86400001): after-ms must be between 1 and 86400000" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (after-ms (random 1 2.5)))"),
		  "random bounds must be whole numbers" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (after-ms (random 10 5)))"),
		  "MIN must not be greater than MAX" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle)"),
		  "(transition idle idle): transition has no trigger" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (when #t) (when #f))"),
		  "duplicate (when ...) trigger" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (after-ms 1) (after-ms 2))"),
		  "duplicate (after-ms ...) trigger" },
		{ "(device (unit-id 1) (holding 1)"
		    " (machine m (initial idle) (state idle) (state-register (holding 1))))",
		  "state-register must immediately follow" },
		{ "(device (unit-id 1) (holding 1)"
		    " (machine m (initial idle) (state-register (holding 1))"
		    "  (state idle (on-enter (set! (holding 1) 1)))))",
		  "state register cannot name a point driven by a machine" },
		{ "(device (unit-id 1) (holding 1)"
		    " (machine m (initial idle) (on-write (holding 1) (set! (holding 0) 1))"
		    "  (state idle)))",
		  "on-write must follow states and precede transitions" },
		{ "(device (unit-id 1) (holding 0) (holding 1)"
		    " (machine m (initial idle) (state idle)"
		    "  (on-write (holding 1) (set! (holding 0) 1))"
		    "  (on-write (holding 1) (set! (holding 0) 2))))",
		  "on-write is given more than once for the same point" },
		{ "(device (unit-id 1) (holding 0) (holding 1)"
		    " (machine m (initial idle)"
		    "  (state idle (on-enter (set! (holding 1) 1)))"
		    "  (on-write (holding 1) (set! (holding 0) 2))))",
		  "on-write cannot watch a machine-driven point" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind delay) (delay-ms 10))"
		    "               (fault (kind delay) (delay-ms 20)))"),
		  "state 'idle' already has a (fault (kind delay)" },
		{ "(server (mode ascii-pty))"
		    " (device (unit-id 1) (holding 0)"
		    "  (machine m (initial idle)"
		    "   (state idle (fault (kind disconnect) (percent 100)))))",
		  "(fault (kind disconnect)) is not valid in ascii-pty mode" },
		{ "(server (mode rtu-pty))"
		    " (device (unit-id 1) (holding 0)"
		    "  (machine m (initial idle)"
		    "   (state idle) (state bad (fault (kind disconnect) (percent 100)))))",
		  "(fault (kind disconnect)) is not valid in rtu-pty mode" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (var n 0) (state-register (holding 0))"
		    "  (state idle)))",
		  "state-register must immediately follow" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (state-register (holding 0))"
		    "  (state-register (holding 0)) (state idle)))",
		  "state-register must immediately follow" },
		{ "(device (unit-id 1) (coil 0)"
		    " (machine m (initial idle) (state-register (coil 0)) (state idle)))",
		  "state register must name a holding or input point" },
		{ "(device (unit-id 1) (holding 0 (type u32))"
		    " (machine m (initial idle) (state-register (holding 1)) (state idle)))",
		  "state register must name the first address of a configured point" },
		{ "(device (unit-id 1)"
		    " (holding 0 (ramp (from 0) (to 10) (every-ms 100)))"
		    " (machine m (initial idle) (state-register (holding 0)) (state idle)))",
		  "state register cannot name a ramp or modeled point" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (state-register (holding 0))"
		    "  (state idle) (state done (on-enter (set! (holding 0) 1)))))",
		  "state register cannot name a point driven by a machine" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine a (initial idle) (state-register (holding 0)) (state idle))"
		    " (machine b (initial idle) (state-register (holding 0)) (state idle)))",
		  "state register point is already used by another machine" },
		{ "(device (unit-id 1) (holding 0) (holding 1)"
		    " (machine m (initial idle) (state-register (holding 1))"
		    "  (state idle) (on-write (holding 1) (set! (holding 0) 1))))",
		  "on-write cannot watch a machine-driven point" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle)"
		    "  (on-write (holding 0) (set! (holding 0) 1)) (state idle)))",
		  "on-write must follow states and precede transitions" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (state idle)"
		    "  (transition idle idle (after-ms 1))"
		    "  (on-write (holding 0) (set! (holding 0) 1))))",
		  "on-write must follow states and precede transitions" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (state idle)"
		    "  (on-write (holding 0) (set! (holding 0) 1)) (state done)))",
		  "state must precede on-write handlers" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (state idle)"
		    "  (on-write (holding 1) (set! (holding 0) 1))))",
		  "on-write target must name the first address of a configured point" },
		{ "(device (unit-id 1) (holding 0 (type u32))"
		    " (machine m (initial idle) (state idle)"
		    "  (on-write (holding 1) (set! (holding 0) 1))))",
		  "on-write target must name the first address of a configured point" },
		{ "(device (unit-id 1) (holding 0)"
		    " (machine m (initial idle) (state idle)"
		    "  (on-write (holding 0))))",
		  "expected (on-write POINT STATEMENT ...)" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind exception) (percent 100)))"),
		  "missing required option (code ...)" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind delay) (percent 100)))"),
		  "missing required option (delay-ms ...)" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind drop) (code 2)))"),
		  "code is only valid for kind exception" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind drop) (delay-ms 10)))"),
		  "delay-ms is only valid for kind delay" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind exception) (code 9)))"),
		  "code 9 is reserved" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind drop) (percent 101)))"),
		  "must be between 0 and 100" },
		{ MACHINE_DEVICE("m (initial idle)"
		    " (state idle (fault (kind bogus) (percent 10)))"),
		  "expected one of: delay, drop, exception, corrupt, disconnect" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

typedef struct expr_case_t expr_case_t;
struct expr_case_t
{
	const char*	expr;
	expr_kind_t	kind;
	val_type_t	type;
	u16		nodes;
};

static bool
load_expr_case(const expr_case_t* ec, config_t* config, char* err, u32 err_size)
{
	char text[512];

	snprintf(text, sizeof(text),
		 "(device (unit-id 1) (coil 0) (discrete 0) (input 0) (holding 0)"
		 " (machine m (initial idle) (var n 0) (var b #f)"
		 "  (state idle (on-enter (set! %s %s)))))",
		 ec->type == VAL_NUM ? "n" : "b", ec->expr);
	return load_text(text, config, err, err_size);
}

TEST(ConfigMachineExpressionHeadsCompileFromTable)
{
	static const expr_case_t CASES[] = {
		{ "(coil 0)",       EXPR_POINT,      VAL_BOOL, 1 },
		{ "(discrete 0)",   EXPR_POINT,      VAL_BOOL, 1 },
		{ "(input 0)",      EXPR_POINT,      VAL_NUM,  1 },
		{ "(holding 0)",    EXPR_POINT,      VAL_NUM,  1 },
		{ "(now-ms)",       EXPR_NOW_MS,     VAL_NUM,  1 },
		{ "(elapsed-ms)",   EXPR_ELAPSED_MS, VAL_NUM,  1 },
		{ "(+ 1 2)",        EXPR_ADD,        VAL_NUM,  3 },
		{ "(- 1 2)",        EXPR_SUB,        VAL_NUM,  3 },
		{ "(* 1 2)",        EXPR_MUL,        VAL_NUM,  3 },
		{ "(/ 1 2)",        EXPR_DIV,        VAL_NUM,  3 },
		{ "(min 1 2)",      EXPR_MIN,        VAL_NUM,  3 },
		{ "(max 1 2)",      EXPR_MAX,        VAL_NUM,  3 },
		{ "(< 1 2)",        EXPR_LT,         VAL_BOOL, 3 },
		{ "(<= 1 2)",       EXPR_LE,         VAL_BOOL, 3 },
		{ "(> 1 2)",        EXPR_GT,         VAL_BOOL, 3 },
		{ "(>= 1 2)",       EXPR_GE,         VAL_BOOL, 3 },
		{ "(= 1 2)",        EXPR_EQ,         VAL_BOOL, 3 },
		{ "(and #t #f)",    EXPR_AND,        VAL_BOOL, 3 },
		{ "(or #t #f)",     EXPR_OR,         VAL_BOOL, 3 },
		{ "(not #f)",       EXPR_NOT,        VAL_BOOL, 2 },
		{ "(if #t 1 2)",    EXPR_IF,         VAL_NUM,  4 },
	};

	for (u32 i = 0; i < ARRAY_COUNT(CASES); i++)
	{
		config_t		config = { 0 };
		char			err[256] = "";
		const machine_def_t*	def = NULL;

		ASSERT_SUCCESS(load_expr_case(&CASES[i], &config, err, sizeof(err)));
		ASSERT_SUCCESS(config.device_count == 1 &&
			       config.devices[0]->machines != NULL &&
			       config.devices[0]->machines->count == 1);
		def = &config.devices[0]->machines->defs[0];
		ASSERT_EQ(def->node_count == CASES[i].nodes);
		ASSERT_EQ(def->nodes[def->node_count - 1].kind == CASES[i].kind);
	}
}

TEST(ConfigMachineRejectsLimitViolations)
{
	char	text[16384];

#define M_HEAD "(device (unit-id 1) (holding 0) (machine m (initial idle)"
	EXPECT_ERROR(gen_text(text, sizeof(text), M_HEAD, " (var v@ 0)", MACHINE_MAX_VARS + 1, " (state idle)))"),
		     "MACHINE_MAX_VARS exceeded");
	EXPECT_ERROR(gen_text(text, sizeof(text), M_HEAD, " (state s@)", MACHINE_MAX_STATES + 1, "))"),
		     "MACHINE_MAX_STATES exceeded");
	EXPECT_ERROR(gen_text(text, sizeof(text), M_HEAD " (state idle)", " (transition idle idle (after-ms 1))",
			      MACHINE_MAX_TRANSITIONS + 1, "))"),
		     "MACHINE_MAX_TRANSITIONS exceeded");
	EXPECT_ERROR(gen_text(text, sizeof(text), M_HEAD " (var n 0) (state idle (on-enter", " (set! n 1)",
			      MACHINE_MAX_STATEMENTS + 1, "))))"),
		     "MACHINE_MAX_STATEMENTS exceeded");
	EXPECT_ERROR(gen_text(text, sizeof(text), M_HEAD " (var n 0) (state idle (on-enter",
			      " (set! n (+ (+ (+ 1 2) 3) 4))", 120, "))))"),
		     "MACHINE_MAX_EXPR_NODES exceeded");
#undef M_HEAD
	EXPECT_ERROR(gen_text(text, sizeof(text), "(device (unit-id 1) (holding 0)",
			      " (machine m@ (initial idle) (state idle (on-enter (set! (holding 0) 1))))",
			      MACHINE_MAX_MACHINES + 1, ")"),
		     "(machine m8 (initial idle) (state idle (on-enter (set! (holding 0) 1)))): "
		     "too many machines on one device (max 8)");
}

TEST(ConfigMachinePhase2Loads)
{
	config_t	config = { 0 };
	device_t*	dev = NULL;
	const machine_def_t* def = NULL;
	fault_set_t	effective = { 0 };
	u16		word = 3;
	u16		out = 0;

	config = load_ok("(device (unit-id 1) (holding 0) (holding 1) (holding 2)"
		     " (machine m (initial idle)"
		     "  (state-register (holding 1))"
		     "  (state idle (fault (kind drop) (percent 100)))"
		     "  (state done (fault (kind exception) (code 4)))"
		     "  (on-write (holding 2) (set! (holding 0) 9))"
		     "  (transition idle done (after-ms 100))))");
	dev = config.devices[0];
	def = &dev->machines->defs[0];
	ASSERT_EQ(def->has_state_register);
	ASSERT_EQ(def->state_register_space == SPACE_HOLDING &&
		  def->state_register_address == 1);
	ASSERT_EQ(def->write_handler_count == 1);
	ASSERT_EQ(def->write_handlers[0].space == SPACE_HOLDING &&
		  def->write_handlers[0].address == 2);
	ASSERT_EQ(def->states[0].faults.present == FAULT_BIT(FAULT_DROP));
	ASSERT_EQ(def->states[0].faults.by_kind[FAULT_DROP].percent == 100);
	ASSERT_EQ(def->states[1].faults.present == FAULT_BIT(FAULT_EXCEPTION));
	ASSERT_EQ(def->states[1].faults.by_kind[FAULT_EXCEPTION].exception_code == 4);
	ASSERT_EQ(def->states[1].faults.by_kind[FAULT_EXCEPTION].percent == 100);
	ASSERT_EQ(dev->points[1].kind == POINT_MACHINE);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 0);
	effective = device_effective_faults(dev);
	ASSERT_EQ(effective.present == FAULT_BIT(FAULT_DROP));
	ASSERT_EQ(effective.by_kind[FAULT_DROP].percent == 100);

	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 2, 1, &word));
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 9);
	ASSERT_EQ(dev->machines->current[0] == 0); /* on-write does not fire transitions */

	device_tick(dev, dev->start_ms + 100);
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 1);
	effective = device_effective_faults(dev);
	ASSERT_EQ(effective.present == FAULT_BIT(FAULT_EXCEPTION));
	ASSERT_EQ(effective.by_kind[FAULT_EXCEPTION].exception_code == 4);
}

TEST(ConfigMachineInputStateRegisterLoads)
{
	config_t	config = { 0 };
	device_t*	dev = NULL;
	u16		out = 0;

	config = load_ok("(device (unit-id 1) (input 3)"
		     " (machine m (initial idle) (state-register (input 3))"
		     "  (state idle) (state done)"
		     "  (transition idle done (after-ms 5))))");
	dev = config.devices[0];
	ASSERT_EQ(dev->machines->defs[0].state_register_space == SPACE_INPUT);
	ASSERT_EQ(dev->machines->defs[0].state_register_address == 3);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 3, 1, &out));
	ASSERT_EQ(out == 0);
	device_tick(dev, dev->start_ms + 5);
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 3, 1, &out));
	ASSERT_EQ(out == 1);
}

TEST(ConfigMachinePhase2Ordering)
{
	static const struct cfg_case cases[] = {
		{ MACHINE_DEVICE("m (state idle)"), "expected (initial STATE)" },
		{ MACHINE_DEVICE("m (initial idle) (state idle) (var n 0)"),
		  "var must precede states and transitions" },
		{ MACHINE_DEVICE("m (initial idle) (state idle)"
		    " (transition idle idle (after-ms 1)) (state done)"),
		  "state must precede transitions" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigMachineAcceptsTcpLikeDisconnectOverlay)
{
	static const char* const MODES[] = { "tcp", "rtu-tcp", "ascii-tcp" };
	config_t	config = { 0 };
	char		text[256] = "";

	for (u32 i = 0; i < ARRAY_COUNT(MODES); i++)
	{
		snprintf(text, sizeof(text),
			 "(server (mode %s))"
			 " (device (unit-id 1) (holding 0)"
			 "  (machine m (initial idle)"
			 "   (state idle (fault (kind disconnect) (percent 100)))))",
			 MODES[i]);
		config = load_ok(text);
		ASSERT_EQ(device_machine_fault_present(config.devices[0], FAULT_DISCONNECT));
	}
}

/* The three datagram modes share their option and fault rules; '@' in the rows is the mode name. */
TEST(ConfigDatagramModes)
{
	static const struct {
		const char*	name;
		server_mode_t	mode;
		u32		first_unit;
		u32		last_unit;
		const char*	extra_option;	/* accepted only by rtu-udp */
	} modes[] = {
		{ "udp",       SERVER_MODE_UDP,       0, 255, "" },
		{ "ascii-udp", SERVER_MODE_ASCII_UDP, 1, 247, "" },
		{ "rtu-udp",   SERVER_MODE_RTU_UDP,   1, 247, " (broadcast-execute on)" },
	};
	static const struct cfg_case common[] = {
		{ "(server (mode @) (idle-timeout-ms 100))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (baud 9600))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (frame-gap-ms 20))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (strict-t15 on))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (strict-t15 off))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (parity none))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (stop-bits 1))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (data-bits 8))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (tls))" DEV, WITH_TLS ? "is not valid in @ mode" : "built without TLS support" },
		{ "(server (mode @) (path \"/dev/ttyS0\"))" DEV, "(path ...) is only valid" },
		{ "(server (mode @) (bind \"localhost\"))" DEV, "not a valid IPv4 or IPv6 address" },
		{ "(server (mode @)) (device (unit-id 1) (fault (kind disconnect)) (holding 0))",
		  "(fault (kind disconnect)) is not valid in @ mode" },
		{ "(server (mode @)) (device (unit-id 1) (holding 0)"
		    " (fault (kind drop) (percent 5)) (fault (kind delay) (delay-ms 10))"
		    " (fault (kind exception) (code 6) (percent 5)) (fault (kind corrupt) (mode register))"
		    " (fault (kind restart) (percent 1)) (fault (kind sensor-disconnect) (percent 1)))",
		  NULL },
	};
	static const struct cfg_case no_broadcast[] = {
		{ "(server (mode @) (broadcast-execute on))" DEV, "is not valid in @ mode" },
		{ "(server (mode @) (broadcast-execute off))" DEV, "is not valid in @ mode" },
	};
	static const struct cfg_case serial_units[] = {
		{ "(server (mode @)) (device (unit-id 0) (holding 0))", "is not valid for RTU/ASCII" },
		{ "(server (mode @)) (device (unit-id 248) (holding 0))", "is not valid for RTU/ASCII" },
		{ "(server (mode @)) (device (unit-id 1) (holding 0) (fault (kind corrupt) (mode checksum)))", NULL },
		{ "(server (mode @)) (device (unit-id 1) (holding 0)"
		    " (fault (kind drop) (percent 5)) (fault (kind delay) (delay-ms 10))"
		    " (fault (kind exception) (code 6) (percent 5)) (fault (kind corrupt) (mode checksum))"
		    " (fault (kind restart) (percent 1)) (fault (kind sensor-disconnect) (percent 1)))",
		  NULL },
	};
	static const struct cfg_case udp_checksum[] = {
		{ "(server (mode @)) (device (unit-id 1) (fault (kind corrupt) (mode checksum)) (holding 0))",
		  "checksum is not valid in @ mode" },
	};
	char	text[256];

	for (u32 m = 0; m < ARRAY_COUNT(modes); m++)
	{
		bool		udp = modes[m].mode == SERVER_MODE_UDP;
		config_t	config;

		snprintf(text, sizeof(text), "(server (mode %s))" DEV, modes[m].name);
		config = load_ok(text);
		ASSERT_EQ(config.servers[0].mode == modes[m].mode);
		ASSERT_EQ(config.servers[0].port == 1502);
		ASSERT_EQ(strcmp(config.servers[0].bind_address, "127.0.0.1") == 0);
		ASSERT_EQ(config.servers[0].broadcast_execute == false);

		snprintf(text, sizeof(text),
			 "(server (mode %s) (port 1503) (bind \"::1\") (seed 9)%s)"
			 " (device (unit-id %u) (holding 0)) (device (unit-id %u) (holding 0))",
			 modes[m].name, modes[m].extra_option, modes[m].first_unit, modes[m].last_unit);
		config = load_ok(text);
		ASSERT_EQ(config.servers[0].mode == modes[m].mode && config.servers[0].port == 1503 && config.servers[0].seed == 9);
		ASSERT_EQ(strcmp(config.servers[0].bind_address, "::1") == 0);
		ASSERT_EQ(config.device_count == 2);
		ASSERT_EQ(config.servers[0].broadcast_execute == (modes[m].extra_option[0] != '\0'));

		run_mode_cases(modes[m].name, common, ARRAY_COUNT(common));
		if (modes[m].mode != SERVER_MODE_RTU_UDP)
			run_mode_cases(modes[m].name, no_broadcast, ARRAY_COUNT(no_broadcast));
		if (udp)
			run_mode_cases(modes[m].name, udp_checksum, ARRAY_COUNT(udp_checksum));
		else
			run_mode_cases(modes[m].name, serial_units, ARRAY_COUNT(serial_units));
	}
}

TEST(ConfigHttpEndpointDefaultsAndAllModes)
{
	config_t	config = load_ok("(device (unit-id 1) (holding 0))");
	ASSERT_EQ(!config.servers[0].http_enabled);
	config = load_ok("(server (http))" DEV);
	ASSERT_EQ(config.servers[0].http_enabled && config.servers[0].http_port == 9102);
	ASSERT_EQ(strcmp(config.servers[0].http_bind_address, "127.0.0.1") == 0);
	ASSERT_EQ(!config.servers[0].http_control);
	config = load_ok("(server (http (control on)))" DEV);
	ASSERT_EQ(config.servers[0].http_enabled && config.servers[0].http_control);
	config = load_ok("(server (http (control off)))" DEV);
	ASSERT_EQ(config.servers[0].http_enabled && !config.servers[0].http_control);
	config = load_ok("(server (mode rtu-pty) (http (port 9200) (bind \"::\")))"
		     " (device (unit-id 1) (holding 0))");
	ASSERT_EQ(config.servers[0].http_enabled && config.servers[0].http_port == 9200 && strcmp(config.servers[0].http_bind_address, "::") == 0);
	/* Different protocols may share a number. */
	config = load_ok("(server (mode udp) (port 9102) (http))" DEV);
}

TEST(ConfigHttpEndpointRejectsBadValues)
{
	static const struct cfg_case cases[] = {
		{ "(server (http (port 0)))" DEV, "port" },
		{ "(server (http (bind \"localhost\")))" DEV,
		  "http: bind address 'localhost' is not a valid IPv4 or IPv6 address" },
		{ "(server (http (control maybe)))" DEV, "control" },
		{ "(server (http (control 2)))" DEV, "control" },
		{ "(server (http (path \"/x\")))" DEV, "path" },
		{ "(server (port 1502) (http (port 1502)))" DEV,
		  "http: port 1502 is already the Modbus server port" },
		{ "(server (mode rtu-tcp) (port 9102) (http))" DEV,
		  "http: port 9102 is already the Modbus server port" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigHttpUiAndAllowedHosts)
{
	config_t	config = load_ok("(server (http))" DEV);
	ASSERT_EQ(!config.servers[0].http_ui && config.servers[0].http_allowed_hosts[0] == '\0');
	config = load_ok("(server (http (ui on)))" DEV);
	ASSERT_EQ(config.servers[0].http_ui && config.servers[0].http_control);	/* the UI needs the control API */
	config = load_ok("(server (http (ui off) (control off)))" DEV);
	ASSERT_EQ(!config.servers[0].http_ui && !config.servers[0].http_control);
	config = load_ok("(server (http (allowed-host \"sim.lab\") (allowed-host \"mogbus-1\")))"
		     " (device (unit-id 1) (holding 0))");
	ASSERT_EQ(strcmp(config.servers[0].http_allowed_hosts, "sim.lab,mogbus-1") == 0);
	config = load_ok("(server (http (allowed-host \"a\") (allowed-host \"b\") (allowed-host \"c\")"
		     " (allowed-host \"d\") (allowed-host \"e\") (allowed-host \"f\") (allowed-host \"g\")"
		     " (allowed-host \"h\")))" DEV);
	ASSERT_EQ(strcmp(config.servers[0].http_allowed_hosts, "a,b,c,d,e,f,g,h") == 0);
	static const struct cfg_case cases[] = {
		{ "(server (http (allowed-host \"a\") (allowed-host \"b\") (allowed-host \"c\")"
		    " (allowed-host \"d\") (allowed-host \"e\") (allowed-host \"f\") (allowed-host \"g\")"
		    " (allowed-host \"h\") (allowed-host \"i\")))" DEV,
		  "at most 8 allowed-host names" },
		{ "(server (http (allowed-host \"\")))" DEV, "allowed-host" },
		{ "(server (http (allowed-host \"a:80\")))" DEV, "allowed-host" },
		{ "(server (http (allowed-host a)))" DEV, "allowed-host" },
		{ "(server (http (ui on) (ui off)))" DEV, "more than once" },
		{ "(server (http (ui maybe)))" DEV, "ui" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigShowRegisters)
{
	config_t config = load_ok("(server (mode tcp))" DEV);

	ASSERT_EQ(config.servers[0].show_registers_ms == 0);
	config = load_ok("(server (mode tcp) (show-registers 250))" DEV);
	ASSERT_EQ(config.servers[0].show_registers_ms == 250);
	config = load_ok("(server (mode udp) (show-registers 10))" DEV);
	ASSERT_EQ(config.servers[0].show_registers_ms == 10);
	config = load_ok("(server (mode rtu-pty) (show-registers 60000))" DEV);
	ASSERT_EQ(config.servers[0].show_registers_ms == 60000);
	static const struct cfg_case cases[] = {
		{ "(server (show-registers 9))" DEV, "show-registers" },
		{ "(server (show-registers 60001))" DEV, "show-registers" },
		{ "(server (show-registers fast))" DEV, "show-registers" },
		{ "(server (show-registers 100) (show-registers 200))" DEV, "more than once" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigUserDefinedFunctionCodes)
{
	config_t			config = { 0 };
	const device_custom_fc_t*	fc;

	config = load_ok("(device (unit-id 1) (holding 0)"
		     " (function-code #x41 (name \"Vendor status\")"
		     "   (on \"01 0a\" (reply \"00 12 34\"))"
		     "   (on \"02\" (registers holding 0 1))"
		     "   (on \"\" (exception 4)))"
		     " (function-code 100 (echo)))");
	ASSERT_EQ(config.devices[0]->custom_fc_count == 2);
	fc = device_find_custom_fc(config.devices[0], 0x41);
	ASSERT_EQ(fc != NULL && strcmp(fc->name, "Vendor status") == 0 && fc->case_count == 3);
	ASSERT_EQ(fc->cases[0].prefix_len == 2 && fc->cases[0].prefix[0] == 0x01 && fc->cases[0].prefix[1] == 0x0A);
	ASSERT_EQ(fc->cases[0].action == CUSTOM_REPLY && fc->cases[0].reply_len == 3 && fc->cases[0].reply[2] == 0x34);
	ASSERT_EQ(fc->cases[1].action == CUSTOM_REGISTERS && fc->cases[1].space == SPACE_HOLDING &&
		  fc->cases[1].count == 1);
	ASSERT_EQ(fc->cases[2].prefix_len == 0 && fc->cases[2].action == CUSTOM_EXCEPTION &&
		  fc->cases[2].exception_code == 4);
	fc = device_find_custom_fc(config.devices[0], 100);
	ASSERT_EQ(fc != NULL && fc->case_count == 1 && fc->cases[0].prefix_len == 0 &&
		  fc->cases[0].action == CUSTOM_ECHO);
}

TEST(ConfigUserDefinedFunctionCodeErrors)
{
	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (holding 0) (function-code 3 (echo)))",
		  "function code 3 (0x03) is implemented by mogbus and cannot be redefined" },
		{ "(device (unit-id 1) (holding 0) (function-code 128 (echo)))", "CODE 1-127" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41))", "needs at least one" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (reply \"1\")))",
		  "is not a list of hex bytes" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (on \"zz\" (echo))))",
		  "is not a list of hex bytes" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (registers holding 0 126)))", "COUNT 1-125" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (registers coil 65535 2)))", "below 65536" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (registers tables 0 1)))",
		  "coil, discrete, input or holding" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (exception 0)))", "exception" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (frob)))", "expected (reply" },
		{ "(device (unit-id 1) (holding 0) (function-code #x41 (echo)) (function-code #x41 (echo)))",
		  "already defined" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigBuiltinFunctionCodesMatchThePduHandler)
{
	for (u32 fc = 0; fc < 256; fc++)
		ASSERT_EQ(config_fc_is_builtin((u8)fc) == modbus_fc_is_builtin((u8)fc));
}

TEST(ConfigParsesSineLagAndNoise)
{
	config_t	config = { 0 };
	device_t*	dev = NULL;
	u16		w = 0;

	config = load_ok("(device (unit-id 1)"
		     " (input 0 (type s16) (lag (target (holding 1)) (tau-ms 1000) (initial 200)))"
		     " (holding 1 (static 300))"
		     " (input 1 (sine (offset 10) (amplitude 5) (period-ms 4000) (phase-deg 0)))"
		     " (input 2 (lag (target 50) (tau-ms 1000) (gain 2) (offset 1)) (noise (amplitude 1)))"
		     " (input 3 (static 7) (noise (amplitude 2) (every-ms 50) (distribution gaussian)))"
		     " (input 4 (ramp (from 0) (to 10)) (noise (amplitude 0.5) (distribution uniform))))");
	dev = config.devices[0];
	ASSERT_EQ(dev->points[0].kind == POINT_MODEL && dev->points[0].model.shape == MODEL_LAG);
	ASSERT_EQ(dev->points[4].kind == POINT_MODEL && dev->points[4].model.noise.gaussian);
	ASSERT_EQ(dev->points[5].kind == POINT_RAMP && dev->points[5].model.noise.every_ms == 1000);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 0, 1, &w));
	ASSERT_EQ(w == 200);
	device_tick(dev, dev->start_ms + 1000);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 0, 1, &w));
	ASSERT_EQ(w == 263);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 1, 1, &w));
	ASSERT_EQ(w == 15);

	/* a lag without (initial ...) starts at its goal, even if the target is declared later */
	config = load_ok("(device (unit-id 1) (input 0 (lag (target (holding 1)) (tau-ms 1000)))"
		     " (holding 1 (static 300)))");
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_INPUT, 0, 1, &w));
	ASSERT_EQ(w == 300);
}

TEST(ConfigMachinesCannotDriveModeledPoints)
{
	EXPECT_ERROR("(device (unit-id 1)"
		     " (holding 0 (sine (offset 0) (amplitude 1) (period-ms 100)))"
		     " (machine m (initial idle)"
		     "  (state idle (on-enter (set! (holding 0) 3)))))",
		     "machine cannot drive a ramp or modeled point");
}

TEST(ConfigParsesDegradation)
{
	config_t	config = { 0 };
	device_t*	dev = NULL;

	config = load_ok("(device (unit-id 1)"
		     " (input 0 (static 10) (degradation (drift-per-hour -0.5)))"
		     " (input 1 (sine (offset 0) (amplitude 1) (period-ms 100)) (noise (amplitude 1))"
		     "  (degradation (after-ms 60000) (noise-growth-per-hour 2) (stuck-after-ms 0) (fail-after-ms 4294967295)))"
		     " (holding 2 (ramp (from 0) (to 10)) (degradation (after-ms 5) (drift-per-hour 1)))"
		     " (coil 0 (static on) (degradation (stuck-after-ms 100) (fail-after-ms 0)))"
		     " (discrete 0 (static off) (degradation (fail-after-ms 5))))");
	dev = config.devices[0];
	ASSERT_EQ(dev->points[0].kind == POINT_MODEL &&
		  dev->points[0].model.degradation.drift_per_hour == -0.5 &&
		  !dev->points[0].model.degradation.has_stuck &&
		  !dev->points[0].model.degradation.has_fail);
	ASSERT_EQ(dev->points[1].model.degradation.after_ms == 60000 &&
		  dev->points[1].model.degradation.noise_growth_per_hour == 2 &&
		  dev->points[1].model.degradation.has_stuck &&
		  dev->points[1].model.degradation.stuck_after_ms == 0 &&
		  dev->points[1].model.degradation.has_fail &&
		  dev->points[1].model.degradation.fail_after_ms == 4294967295u);
	ASSERT_EQ(dev->points[2].kind == POINT_RAMP &&
		  dev->points[2].model.degradation.drift_per_hour == 1);
	ASSERT_EQ(dev->points[3].kind == POINT_MODEL &&
		  dev->points[3].model.degradation.has_fail &&
		  dev->points[3].model.degradation.fail_after_ms == 0);
	/* fail-after-ms 0 fails from the first request */
	ASSERT_EQ(device_range_failed(dev, SPACE_COIL, 0, 1));
	ASSERT_EQ(!device_range_failed(dev, SPACE_INPUT, 0, 2));
	ASSERT_EQ(!device_range_failed(dev, SPACE_DISCRETE, 0, 1));
	device_tick(dev, dev->start_ms + 5);
	ASSERT_EQ(device_range_failed(dev, SPACE_DISCRETE, 0, 1));
}

TEST(ConfigParsesSixtyFourBitTypesWordOrderAndByteOrder)
{
	config_t	config = { 0 };
	u16		w[4] = { 0 };

	config = load_ok("(device (unit-id 1)\n"
		     "  (holding 0 (type u64) (static #x0102030405060708))\n"
		     "  (holding 4 (type u64) (word-order low-first) (static #x0102030405060708))\n"
		     "  (holding 8 (type s64) (byte-order little) (static -2))\n"
		     "  (holding 12 (type float64) (static 1.0))\n"
		     "  (holding 16 (type float32) (byte-order little) (static 1.5))\n"
		     "  (holding 18 (type u16) (byte-order little) (static #x1234))\n"
		     "  (holding 20 (type float64) (static +nan.0)))");
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 4, w));
	ASSERT_EQ(w[0] == 0x0102 && w[1] == 0x0304 && w[2] == 0x0506 && w[3] == 0x0708);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 4, 4, w));
	ASSERT_EQ(w[0] == 0x0708 && w[1] == 0x0506 && w[2] == 0x0304 && w[3] == 0x0102);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 8, 4, w));
	ASSERT_EQ(w[0] == 0xFFFF && w[1] == 0xFFFF && w[2] == 0xFFFF && w[3] == 0xFEFF);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 12, 4, w));
	ASSERT_EQ(w[0] == 0x3FF0 && w[1] == 0 && w[2] == 0 && w[3] == 0);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 16, 2, w));
	ASSERT_EQ(w[0] == 0xC03F && w[1] == 0);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 18, 1, w));
	ASSERT_EQ(w[0] == 0x3412);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 20, 1, w));
	ASSERT_EQ(w[0] == 0x7FF8);
}

TEST(ConfigStaticSixtyFourBitIntegersAreExact)
{
	config_t	config = { 0 };
	u16		w[4] = { 0 };

	/* s7 reads integers as 64-bit: 2^63-1 and (as hex) 2^64-1 do not go through a double. */
	config = load_ok("(device (unit-id 1)\n"
		     "  (holding 0 (type s64) (static 9223372036854775807))\n"
		     "  (holding 4 (type u64) (static #xFFFFFFFFFFFFFFFF))\n"
		     "  (holding 8 (type s64) (static -9223372036854775808))\n"
		     "  (holding 12 (type u64) (static 9007199254740993)))");
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 4, w));
	ASSERT_EQ(w[0] == 0x7FFF && w[1] == 0xFFFF && w[2] == 0xFFFF && w[3] == 0xFFFF);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 4, 4, w));
	ASSERT_EQ(w[0] == 0xFFFF && w[1] == 0xFFFF && w[2] == 0xFFFF && w[3] == 0xFFFF);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 8, 4, w));
	ASSERT_EQ(w[0] == 0x8000 && w[1] == 0 && w[2] == 0 && w[3] == 0);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 12, 4, w));
	ASSERT_EQ(w[0] == 0x0020 && w[1] == 0 && w[2] == 0 && w[3] == 0x0001);	/* 2^53 + 1 */
}

TEST(ConfigParsesStringPoints)
{
	config_t	config = { 0 };
	u16		w[4] = { 0 };
	char		text[16];

	config = load_ok("(device (unit-id 1)\n"
		     "  (holding 0 (type string) (length 3) (static \"ABCDE\"))\n"
		     "  (holding 10 (type string) (length 2) (padding space) (static \"HI\"))\n"
		     "  (input 20 (type string) (length 2) (byte-order little) (static \"ABCD\"))\n"
		     "  (holding 30 (type string) (length 1) (count 3) (static \"X\"))\n"
		     "  (holding 40 (type string) (length 2)))");
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 3, w));
	ASSERT_EQ(w[0] == 0x4142 && w[1] == 0x4344 && w[2] == 0x4500);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 10, 2, w));
	ASSERT_EQ(w[0] == 0x4849 && w[1] == 0x2020);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_INPUT, 20, 2, w));
	ASSERT_EQ(w[0] == 0x4241 && w[1] == 0x4443);
	ASSERT_SUCCESS(device_get_string(config.devices[0], SPACE_HOLDING, 32, text, sizeof(text)));
	ASSERT_EQ(strcmp(text, "X") == 0);
	ASSERT_SUCCESS(device_get_string(config.devices[0], SPACE_HOLDING, 40, text, sizeof(text)));
	ASSERT_EQ(text[0] == '\0');
}

TEST(ConfigRejectsBadPointOptions)
{
	static const struct cfg_case cases[] = {
		{ "(device (unit-id 1) (holding 0 (type string) (static \"AB\")))", "(length" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 0)))", "between 1 and 123" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 124)))", "between 1 and 123" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 1) (static \"ABC\")))",
		  "longer than 2 bytes" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (static \"caf\xc3\xa9\")))",
		  "printable ASCII" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (static \"a\\tb\")))", "printable ASCII" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (static 5)))", "text in double quotes" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (ramp (from 0) (to 1))))",
		  "can only be (static" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (sine (offset 0) (amplitude 1) (period-ms 100))))",
		  "can only be (static" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (static \"A\") (noise (amplitude 1))))",
		  "cannot have (noise" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (word-order low-first)))",
		  "no word order" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2) (padding tab)))", "padding" },
		{ "(device (unit-id 1) (holding 0 (type u16) (padding space)))", "only apply to (type string)" },
		{ "(device (unit-id 1) (holding 0 (type u16) (length 2)))", "only apply to (type string)" },
		{ "(device (unit-id 1) (coil 0 (type string) (length 2)))", "remove (type" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2)) (holding 1 (static 1)))",
		  "already used" },
		{ "(device (unit-id 1) (holding 0 (byte-order middle)))", "byte-order" },
		{ "(device (unit-id 1) (coil 0 (byte-order little)))", "remove (byte-order" },
		{ "(device (unit-id 1) (holding 0 (type u64) (static 1.5) (static 2)))", "static" },
		{ "(device (unit-id 1) (holding 65533 (type u64)))", "does not fit" },
		{ "(device (unit-id 1) (holding 0 (type u16) (static +nan.0)))", "only float32 and float64" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2)) (holding 5)"
		    " (machine m (initial idle)"
		    "  (state idle (on-enter (set! (holding 0) 1)))))",
		  "machine cannot drive a string point" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2)) (holding 5)"
		    " (machine m (initial idle)"
		    "  (state idle (on-enter (when (< (holding 0) 1) (set! (holding 5) 2))))))",
		  "a string point has no number to read" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2))"
		    " (machine m (initial idle) (state-register (holding 0)) (state idle)))",
		  "state register cannot name a string point" },
		{ "(device (unit-id 1) (holding 0 (type string) (length 2)) (holding 5)"
		    " (machine m (initial idle) (state idle)"
		    "  (on-write (holding 0) (set! (holding 5) 1))))",
		  "on-write cannot watch a string point" },
		{ "(device (unit-id 1) (holding 0 (access sometimes)))", "access" },
		{ "(device (unit-id 1) (holding 0 (access)))", "access" },
		{ "(device (unit-id 1) (input 0 (access read-only)))", "input registers are always read-only" },
		{ "(device (unit-id 1) (input 0 (access read-write)))", "remove (access" },
		{ "(device (unit-id 1) (discrete 0 (access read-only)))", "discrete inputs are always read-only" },
		{ "(device (unit-id 1) (holding 0 (clock fortnight)))", "unknown clock field" },
		{ "(device (unit-id 1) (holding 0 (clock)))", "expected (clock FIELD)" },
		{ "(device (unit-id 1) (holding 0 (clock unix)))", "clock unix needs a 32-bit or 64-bit type" },
		{ "(device (unit-id 1) (holding 0 (type float32) (clock year)))",
		  "clock points need an integer register type" },
		{ "(device (unit-id 1) (coil 0 (clock year)))",
		  "coils and discrete inputs can only be (static on/off)" },
		{ "(device (unit-id 1) (holding 0 (clock year) (static 1)))", "only one of" },
		{ "(device (unit-id 1) (holding 0 (clock year) (noise (amplitude 1))))", "cannot have (noise" },
		{ "(device (unit-id 1) (holding 0 (clock year (tz 1))))", "tz" },
		{ "(device (unit-id 1) (holding 0 (clock year)) (machine m (initial idle)"
		    " (state idle (on-enter (set! (holding 0) 3)))))",
		  "machine cannot drive" },
	};
	run_cases(cases, ARRAY_COUNT(cases));
}

TEST(ConfigSixtyFourBitPointsWorkInMachines)
{
	config_t	config = { 0 };
	u16		w[4] = { 0 };

	config = load_ok("(device (unit-id 1) (holding 0 (type u64) (static 5)) (holding 4 (type float64))\n"
		     " (machine m (initial idle)\n"
		     "  (state idle (on-enter (set! (holding 4) (+ (holding 0) 0.5))))))");
	device_tick(config.devices[0], 1);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 4, 4, w));
	ASSERT_EQ(w[0] == 0x4016 && w[1] == 0 && w[2] == 0 && w[3] == 0);	/* 5.5 */
}

TEST(ConfigParsesPointAccess)
{
	config_t	config = { 0 };
	device_t*	dev    = NULL;
	u16		nine   = 9;

	config = load_ok("(device (unit-id 1)\n"
		     "  (holding 0 (access read-only) (type u32) (static 7))\n"
		     "  (holding 2 (access read-write) (static 1))\n"
		     "  (holding 3 (static 1) (count 2) (access read-only))\n"
		     "  (coil 0 (access read-only) (static on) (count 2))\n"
		     "  (coil 2))");
	dev = config.devices[0];
	ASSERT_EQ(dev->points[0].read_only && !dev->points[1].read_only);
	ASSERT_EQ(dev->points[2].read_only && dev->points[3].read_only);
	ASSERT_EQ(dev->points[4].read_only && dev->points[5].read_only && !dev->points[6].read_only);
	ASSERT_FAILURE(device_client_write(dev, SPACE_HOLDING, 3, 1, &nine));
	ASSERT_SUCCESS(device_client_write(dev, SPACE_HOLDING, 2, 1, &nine));
}

TEST(ConfigClockPointsShowTheWallClock)
{
	config_t	config = { 0 };
	device_t*	dev    = NULL;
	u16		out[4];
	u16		nine   = 9;

	config = load_ok("(device (unit-id 1)\n"
		     "  (holding 0 (clock year (utc on)))\n"
		     "  (holding 1 (clock month (utc on)))\n"
		     "  (holding 2 (clock day (utc on)))\n"
		     "  (holding 3 (clock weekday (utc on)))\n"
		     "  (holding 4 (clock hour (utc on)))\n"
		     "  (holding 5 (clock minute (utc on)))\n"
		     "  (holding 6 (clock second (utc on)))\n"
		     "  (input 10 (type u32) (clock unix))\n"
		     "  (input 12 (type s64) (clock unix (utc off))))");
	dev = config.devices[0];
	ASSERT_EQ(dev->points[0].kind == POINT_CLOCK && dev->points[0].clock_utc);
	dev->wall_fixed_ms = 1735685156789LL; /* Tue 2024-12-31 22:45:56 UTC */
	device_tick(dev, dev->start_ms + 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 4, out));
	ASSERT_EQ(out[0] == 2024 && out[1] == 12 && out[2] == 31 && out[3] == 2);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 4, 3, out));
	ASSERT_EQ(out[0] == 22 && out[1] == 45 && out[2] == 56);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 10, 2, out));
	ASSERT_EQ(out[0] == (u16)(1735685156u >> 16) && out[1] == (u16)(1735685156u & 0xFFFF));
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 12, 4, out));
	ASSERT_EQ(out[0] == 0 && out[1] == 0 && out[2] == (u16)(1735685156u >> 16));

	dev->wall_fixed_ms += 2 * 3600 * 1000 + 3000;
	device_tick(dev, dev->start_ms + 2);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, out));
	ASSERT_EQ(out[0] == 2025);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 6, 1, out));
	ASSERT_EQ(out[0] == 59); /* 2025-01-01 00:45:59 */

	/* A client write freezes the point; a restart makes it live again. */
	ASSERT_SUCCESS(device_client_write(dev, SPACE_HOLDING, 6, 1, &nine));
	dev->wall_fixed_ms += 1000;
	device_tick(dev, dev->start_ms + 3);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 6, 1, out));
	ASSERT_EQ(out[0] == 9);
	device_restart(dev, dev->start_ms + 4);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 6, 1, out));
	ASSERT_EQ(out[0] == 0);
}

#ifndef _WIN32
TEST(ConfigClockLocalTimeFollowsTheTimeZone)
{
	config_t	config = { 0 };
	device_t*	dev    = NULL;
	u16		out[2];

	config = load_ok("(device (unit-id 1) (holding 0 (clock hour)) (holding 1 (clock day)))");
	dev = config.devices[0];
	dev->wall_fixed_ms = 1735685156789LL; /* 2024-12-31 22:45:56 UTC */
	setenv("TZ", "UTC-3", 1);
	tzset();
	device_tick(dev, dev->start_ms + 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 2, out));
	ASSERT_EQ(out[0] == 1 && out[1] == 1);
	unsetenv("TZ");
	tzset();
}
#endif


/* One config text of the multi-server table and the error fragment it must fail with (NULL: it must load). */
struct multi_case {
	const char*	name;
	const char*	text;
	const char*	error;
};

#define S(id, extra)	"(server (id \"" id "\") " extra ")"
#define D(unit, servers) "(device (unit-id " #unit ") " servers " (holding 0))"

TEST(ConfigSeveralServersTable)
{
	static const struct multi_case cases[] = {
		{ "two servers, devices assigned",
		  S("a", "(port 1601)") S("b", "(mode udp) (port 1601)") D(1, "(servers \"a\" \"b\")") D(2, "(servers \"b\")"),
		  NULL },
		{ "same unit id on servers that share no device",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers \"a\")") D(1, "(servers \"b\")"),
		  NULL },
		{ "same unit id on a shared server",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers \"a\" \"b\")") D(1, "(servers \"b\")"),
		  "both use unit-id 1" },
		{ "any-unit once per server",
		  S("a", "(port 1601)") S("b", "(port 1602)")
		  "(device (unit-id 1) (any-unit on) (servers \"a\") (holding 0))"
		  "(device (unit-id 2) (any-unit on) (servers \"b\") (holding 0))",
		  NULL },
		{ "any-unit twice on one server",
		  S("a", "(port 1601)") S("b", "(port 1602)")
		  "(device (unit-id 1) (any-unit on) (servers \"a\") (holding 0))"
		  "(device (unit-id 2) (any-unit on) (servers \"a\" \"b\") (holding 0))",
		  "both set any-unit" },
		{ "unit 0 is fine on tcp, not on the rtu server of the same file",
		  S("a", "(port 1601)") S("b", "(mode rtu-tcp) (port 1602)") D(0, "(servers \"a\")") D(3, "(servers \"b\")"),
		  NULL },
		{ "unit 0 on an rtu server",
		  S("a", "(port 1601)") S("b", "(mode rtu-tcp) (port 1602)") D(0, "(servers \"a\" \"b\")"),
		  "not valid for RTU/ASCII" },
		{ "checksum corruption is checked per server",
		  S("a", "(mode rtu-tcp) (port 1601)") S("b", "(port 1602)")
		  "(device (unit-id 1) (servers \"a\") (fault (kind corrupt) (percent 10) (mode checksum)) (holding 0))"
		  D(2, "(servers \"b\")"),
		  NULL },
		{ "checksum corruption on a server without checksum",
		  S("a", "(mode rtu-tcp) (port 1601)") S("b", "(port 1602)")
		  "(device (unit-id 1) (servers \"a\" \"b\") (fault (kind corrupt) (percent 10) (mode checksum)) (holding 0))",
		  "not valid in tcp mode" },
		{ "several servers need ids",
		  S("a", "(port 1601)") "(server (port 1602))" D(1, "(servers \"a\")"),
		  "server 2 has no (id" },
		{ "duplicate ids",
		  S("a", "(port 1601)") S("a", "(port 1602)") D(1, "(servers \"a\")"),
		  "both use id 'a'" },
		{ "bad id characters",
		  S("a b", "(port 1601)") D(1, ""),
		  "must be 1 to 31 letters" },
		{ "a device must name its server",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers \"a\")") D(2, ""),
		  "is not assigned to a server" },
		{ "unknown server",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers \"a\" \"c\")"),
		  "names 'c'" },
		{ "a server without devices",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers \"a\")"),
		  "server 'b' serves no device" },
		{ "servers given twice",
		  S("a", "(port 1601)") S("b", "(port 1602)") "(device (unit-id 1) (servers \"a\") (servers \"b\") (holding 0))",
		  "servers is given more than once" },
		{ "server listed twice",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers \"a\" \"a\")"),
		  "listed twice" },
		{ "empty servers list",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers)"),
		  "at least one server id" },
		{ "servers are strings",
		  S("a", "(port 1601)") S("b", "(port 1602)") D(1, "(servers a)"),
		  "server ids are strings" },
		{ "servers without any (server (id ...))",
		  "(server (port 1601))" D(1, "(servers \"a\")"),
		  "names 'a'" },
		{ "one named server serves every device",
		  S("a", "(port 1601)") D(1, "") D(2, ""),
		  NULL },
		{ "same tcp port",
		  S("a", "(port 1601)") S("b", "(mode rtu-tcp) (port 1601)") D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  "both listen on TCP port 1601" },
		{ "same port, tcp and udp",
		  S("a", "(port 1601)") S("b", "(mode udp) (port 1601)") D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  NULL },
		{ "same port, different loopback addresses",
		  S("a", "(port 1601) (bind \"127.0.0.1\")") S("b", "(port 1601) (bind \"127.0.0.2\")")
		  D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  NULL },
		{ "same port, wildcard and an address",
		  S("a", "(port 1601) (bind \"0.0.0.0\")") S("b", "(port 1601) (bind \"127.0.0.2\")")
		  D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  "both listen on TCP port 1601" },
		{ "same udp port",
		  S("a", "(mode udp) (port 1601)") S("b", "(mode ascii-udp) (port 1601)")
		  D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  "both listen on UDP port 1601" },
		{ "http endpoint against a tcp port",
		  S("a", "(port 1601) (http (port 1700))") S("b", "(port 1700)") D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  "both listen on TCP port 1700" },
		{ "http endpoint against a udp port",
		  S("a", "(port 1601) (http (port 1700))") S("b", "(mode udp) (port 1700)") D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  NULL },
		{ "two http endpoints",
		  S("a", "(port 1601) (http (port 1700))") S("b", "(port 1602) (http (port 1700))")
		  D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  "both listen on TCP port 1700" },
		{ "same serial device",
		  S("a", "(mode rtu-device) (path \"/dev/ttyS9\")") S("b", "(mode ascii-device) (path \"/dev/ttyS9\")")
		  D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  "both use the serial device /dev/ttyS9" },
		{ "two ptys",
		  S("a", "(mode rtu-pty)") S("b", "(mode ascii-pty)") D(1, "(servers \"a\")") D(2, "(servers \"b\")"),
		  NULL },
	};

	for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		config_t	c = { 0 };
		char		e[512] = "";
		bool		ok;

		test_case = cases[i].name;
		ok = load_text(cases[i].text, &c, e, sizeof(e));
		if (cases[i].error)
		{
			ASSERT_EQ(!ok);
			if (!strstr(e, cases[i].error))
				fprintf(stderr, "got: %s\n", e);
			ASSERT_EQ(strstr(e, cases[i].error) != NULL);
		}
		else
		{
			if (!ok)
				fprintf(stderr, "got: %s\n", e);
			ASSERT_EQ(ok);
		}
	}
	test_case = 0;
}

TEST(ConfigServersListsAndDefaults)
{
	config_t	c;

	/* No (server ...): one default server, and devices need no assignment. */
	c = load_ok(DEV);
	ASSERT_EQ(c.server_count == 1 && c.servers[0].id[0] == '\0' && c.servers[0].port == SERVER_DEFAULT_PORT);
	ASSERT_EQ(c.devices[0]->server_count == 0);

	/* One named server is assigned to every device that lists none. */
	c = load_ok("(server (id \"main\") (port 1601))" DEV " (device (unit-id 2) (holding 0))");
	ASSERT_EQ(c.server_count == 1 && strcmp(c.servers[0].id, "main") == 0);
	ASSERT_EQ(c.devices[0]->server_count == 1 && device_serves(c.devices[0], "main") &&
		  device_serves(c.devices[1], "main") && !device_serves(c.devices[0], NULL));

	/* Several servers keep their own settings, in file order. */
	c = load_ok(S("x", "(mode udp) (port 1701) (seed 3)") S("y", "(port 1702) (idle-timeout-ms 500)")
		    D(1, "(servers \"x\" \"y\")"));
	ASSERT_EQ(c.server_count == 2 && c.servers[0].mode == SERVER_MODE_UDP && c.servers[0].port == 1701 &&
		  c.servers[1].mode == SERVER_MODE_TCP && c.servers[1].idle_timeout_ms == 500);
	ASSERT_EQ(device_serves(c.devices[0], "x") && device_serves(c.devices[0], "y") && !device_serves(c.devices[0], "z"));

	/* A device is seeded by the first server (in file order) that serves it. */
	c = load_ok(S("x", "(port 1701) (seed 5)") S("y", "(port 1702) (seed 9)")
		    "(device (unit-id 1) (servers \"y\" \"x\") (holding 0) (machine m (initial idle) (state idle)))"
		    "(device (unit-id 2) (servers \"y\") (holding 0) (machine m (initial idle) (state idle)))");
	ASSERT_EQ(c.devices[0]->machines->seed == 5 && c.devices[1]->machines->seed == 9);
}

TEST(ConfigAtMostEightServers)
{
	char		text[4096] = "";
	char		e[256]     = "";
	config_t	c          = { 0 };
	size_t		n          = 0;

	for (int i = 0; i < 9; i++)
	{
		n += (size_t)snprintf(text + n, sizeof(text) - n, "(server (id \"s%d\") (port %d)) ", i, 1800 + i);
		if (i == 7)
		{
			(void)snprintf(text + n, sizeof(text) - n,
				       "(device (unit-id 1) (servers \"s0\" \"s1\" \"s2\" \"s3\" \"s4\" \"s5\" \"s6\" \"s7\") (holding 0))");
			ASSERT_EQ(load_text(text, &c, e, sizeof(e)));
			ASSERT_EQ(c.server_count == 8);
		}
	}
	snprintf(text + n, sizeof(text) - n, "(device (unit-id 1) (servers \"s0\") (holding 0))");
	ASSERT_EQ(!load_text(text, &c, e, sizeof(e)) && strstr(e, "too many (server") != NULL);
}
