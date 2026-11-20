#include "vtest.h"
#include "config/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Writes `text` to a temp file, loads it, and returns the load result. */
static bool
load_text(const char* text, config_t* config, char* err, u32 err_size)
{
	char		path[]  = "/tmp/modbus-sim-test-XXXXXX";
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
	ok = config_load(persist, scratch, path, config, err, err_size);
	unlink(path);
	arena_release(scratch);
	return ok;
}

TEST(ConfigLoadsServerAndDevice)
{
	config_t	config = { 0 };
	char		err[256] = "";
	u16		out[2] = { 0, 0 };

	ASSERT_SUCCESS(load_text(
		"; a comment\n"
		"(server (port 1600) (bind \"127.0.0.1\") (seed 5))\n"
		"(device (name \"Test\") (unit-id 4)\n"
		"  (holding 0 (static 42))\n"
		"  (input 0 (type float32) (static 1.0))\n"
		"  (coil 3 (static on)))\n", &config, err, sizeof(err)));

	ASSERT_EQ(config.port == 1600 && config.seed == 5);
	ASSERT_EQ(strcmp(config.bind_address, "127.0.0.1") == 0);
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
	char		err[256] = "";
	u16		out[16];

	ASSERT_SUCCESS(load_text("(device (coil 0 (count 16) (static on)) (holding 10 (type u32) (count 2) (static 7)))",
				 &config, err, sizeof(err)));
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_COIL, 0, 16, out));
	ASSERT_EQ(out[15] == 1);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 10, 4, out));
	ASSERT_EQ(out[1] == 7 && out[3] == 7);
	ASSERT_FAILURE(device_read(config.devices[0], SPACE_HOLDING, 14, 1, out));
}

TEST(ConfigDefaults)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.port == CONFIG_DEFAULT_PORT);
	ASSERT_EQ(config.devices[0]->unit_id == 1);
}

TEST(ConfigFaultIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (fault (kind delay) (delay-ms 300) (percent 40)) (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->fault.kind == FAULT_DELAY);
	ASSERT_EQ(config.devices[0]->fault.delay_ms == 300 && config.devices[0]->fault.percent == 40);
}

#define EXPECT_ERROR(text, fragment)							\
	do {										\
		config_t	c = { 0 };						\
		char		e[256] = "";						\
		ASSERT_FAILURE(load_text((text), &c, e, sizeof(e)));			\
		ASSERT_EQ(strstr(e, (fragment)) != NULL);				\
	} while (0)

TEST(ConfigErrorsAreReadable)
{
	EXPECT_ERROR("(device (holding 0 (static 1))", "syntax error");
	EXPECT_ERROR("(device (holding 0 (statik 1)))", "unknown option 'statik'");
	EXPECT_ERROR("(device (holding 70000))", "between 0 and 65535");
	EXPECT_ERROR("(device (holding 0 (static \"x\")))", "expected a number");
	EXPECT_ERROR("(device (coil 0 (ramp (from 0) (to 1))))", "can only be (static on/off)");
	EXPECT_ERROR("(device (holding 0 (ramp (from 0))))", "missing required option (to");
	EXPECT_ERROR("(device (holding 0) (holding 0))", "already used");
	EXPECT_ERROR("(device (fault (kind drop)) (fault (kind drop)))", "only one is allowed");
	EXPECT_ERROR("(device (holding 0) (frobnicate 1))", "unknown item 'frobnicate'");
	EXPECT_ERROR("(server (port 1)) (server (port 2)) (device (holding 0))", "more than one (server");
	EXPECT_ERROR("(device (holding 0 (ramp (from 0) (to 1e30))))", "too large for the step");
	EXPECT_ERROR("(server (port 1))", "no (device");
	EXPECT_ERROR("(device (unit-id 1)) (device (unit-id 1))", "both use unit-id 1");
	EXPECT_ERROR("(oops)", "unknown top-level form 'oops'");
}

TEST(ConfigMissingFileIsAnError)
{
	config_t	config = { 0 };
	char		err[256] = "";
	arena_t*	persist = ARENA_ALLOC();
	arena_t*	scratch = ARENA_ALLOC();

	ASSERT_FAILURE(config_load(persist, scratch, "/nonexistent/file.scm", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "cannot read") != NULL);
	arena_release(persist);
	arena_release(scratch);
}

TEST(ConfigBoolWords)
{
	config_t	config = { 0 };
	char		err[256] = "";
	u16		out = 9;

	ASSERT_SUCCESS(load_text("(device (coil 0 (static yes)) (coil 1 (static #f)) (coil 2 (static 1)))",
				 &config, err, sizeof(err)));
	device_read(config.devices[0], SPACE_COIL, 0, 1, &out);
	ASSERT_EQ(out == 1);
	device_read(config.devices[0], SPACE_COIL, 1, 1, &out);
	ASSERT_EQ(out == 0);
	device_read(config.devices[0], SPACE_COIL, 2, 1, &out);
	ASSERT_EQ(out == 1);
}
