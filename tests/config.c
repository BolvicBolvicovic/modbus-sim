#include "vtest.h"
#include "config/config.h"
#include <math.h>
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

TEST(ConfigRejectsCountExceedingDevicePointLimit)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(device (unit-id 1) (coil 0 (count 8193)))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "8192-point limit") != NULL);

	ASSERT_FAILURE(load_text("(device (unit-id 1) (coil 0 (count 8192)) (coil 8192 (static on)))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "8192-point limit") != NULL);
}

TEST(ConfigDefaults)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.port == CONFIG_DEFAULT_PORT);
	ASSERT_EQ(config.idle_timeout_ms == CONFIG_DEFAULT_IDLE_TIMEOUT_MS);
	ASSERT_EQ(config.frame_gap_ms == 0);
	ASSERT_EQ(config.baud == 0);
	ASSERT_EQ(config.devices[0]->unit_id == 1);
}

TEST(ConfigRampBounceStepLimitBoundaries)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(device (holding 0 (ramp (from 0) (to 4611686018427387904.0) (step 1) (mode bounce))))",
		&config, err, sizeof(err)));
	ASSERT_FAILURE(load_text(
		"(device (holding 0 (ramp (from 0) (to 9223372036854775808.0) (step 1) (mode bounce))))",
		&config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "ramp range is too large for the step") != NULL);
	ASSERT_FAILURE(load_text(
		"(device (holding 0 (ramp (from 0) (to 9223372036854777856.0) (step 1) (mode bounce))))",
		&config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "ramp range is too large for the step") != NULL);
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

TEST(ConfigExceptionStatusIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (exception-status 255) (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->exception_status == 255);

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->exception_status == 0);
}

TEST(ConfigExceptionStatusRejectsOutOfRange)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(device (exception-status 256) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "between 0 and 255") != NULL);
}

TEST(ConfigReportServerIdIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (server-id 42) (run-indicator 0) (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->server_id == 42);
	ASSERT_EQ(config.devices[0]->run_indicator == 0);

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->server_id == 0);
	ASSERT_EQ(config.devices[0]->run_indicator == 0xFF);
}

TEST(ConfigReportServerIdRejectsOutOfRange)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(device (server-id 256) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "between 0 and 255") != NULL);

	ASSERT_FAILURE(load_text("(device (run-indicator 256) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "between 0 and 255") != NULL);
}

TEST(ConfigDeviceIdentificationDefaultsAndOverrides)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_VENDOR_NAME], "modbus-sim") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_PRODUCT_CODE], "modbus-sim") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_MAJOR_MINOR_REVISION], "1.0") == 0);
	ASSERT_EQ(config.devices[0]->device_id[DEVICE_ID_VENDOR_URL][0] == '\0');

	ASSERT_SUCCESS(load_text(
		"(device (vendor-name \"Acme\") (product-code \"P1\") (major-minor-revision \"2.3\")"
		"        (vendor-url \"https://acme.invalid\") (product-name \"Sim\")"
		"        (model-name \"M\") (user-application-name \"App\") (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_VENDOR_NAME], "Acme") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_PRODUCT_CODE], "P1") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_MAJOR_MINOR_REVISION], "2.3") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_VENDOR_URL], "https://acme.invalid") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_PRODUCT_NAME], "Sim") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_MODEL_NAME], "M") == 0);
	ASSERT_EQ(strcmp(config.devices[0]->device_id[DEVICE_ID_USER_APPLICATION_NAME], "App") == 0);
}

TEST(ConfigDeviceIdentificationRejectsTooLong)
{
	config_t	config = { 0 };
	char		err[512] = "";
	char		text[512];
	char		longv[246];

	memset(longv, 'x', 245);
	longv[245] = '\0';
	snprintf(text, sizeof(text), "(device (vendor-name \"%s\") (holding 0))", longv);
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "vendor-name is too long") != NULL);
	ASSERT_EQ(strstr(err, "FC 43 / 0x2B MEI 14") != NULL);
}

TEST(ConfigDeviceIdentificationAcceptsMaxSize)
{
	config_t	config = { 0 };
	char		err[512] = "";
	char		text[512];
	char		maxv[245];

	memset(maxv, 'V', 244);
	maxv[244] = '\0';
	snprintf(text, sizeof(text), "(device (vendor-name \"%s\") (holding 0))", maxv);
	ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strlen(config.devices[0]->device_id[DEVICE_ID_VENDOR_NAME]) == 244);
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
	EXPECT_ERROR("(device (holding 0 (ramp (from 0) (to 9223372036854775808.0) (step 1) (mode bounce))))", "too large for the step");
	EXPECT_ERROR("(device (holding 0 (ramp (from 0) (to 9223372036854777856.0) (step 1) (mode bounce))))", "too large for the step");
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

TEST(ConfigFileRecordsAreParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";
	u16		out[3] = { 0, 0, 0 };

	ASSERT_SUCCESS(load_text("(device"
				 "  (file 1 (record 0 4660 22136))"
				 "  (file 4 (record 10 1 2 3)))",
				 &config, err, sizeof(err)));
	ASSERT_SUCCESS(device_read_file_records(config.devices[0], 1, 0, 2, out));
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678);
	ASSERT_SUCCESS(device_read_file_records(config.devices[0], 4, 10, 3, out));
	ASSERT_EQ(out[0] == 1 && out[1] == 2 && out[2] == 3);
}

TEST(ConfigFileRecordErrors)
{
	EXPECT_ERROR("(device (file 1))", "at least one (record");
	EXPECT_ERROR("(device (file 1 (record 0)))", "(record REC_NO VALUE");
	EXPECT_ERROR("(device (file 1 (record 10000 1)))", "between 0 and 9999");
	EXPECT_ERROR("(device (file 0 (record 0 1)))", "between 1 and 65535");
	EXPECT_ERROR("(device (file 65536 (record 0 1)))", "between 1 and 65535");
	EXPECT_ERROR("(device (file 1 (record 0 65536)))", "between 0 and 65535");
	EXPECT_ERROR("(device (file 1 (record 0 1 2) (record 1 3)))", "overlaps another record");
}

TEST(ConfigFifoQueuesAreParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";
	u16		out[3] = { 0, 0, 0 };
	u16		count  = 0;

	ASSERT_SUCCESS(load_text("(device"
				 "  (fifo 0 (values 4660 22136))"
				 "  (fifo 1000 (values)))",
				 &config, err, sizeof(err)));
	ASSERT_SUCCESS(device_read_fifo(config.devices[0], 0, &count, out));
	ASSERT_EQ(count == 2);
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678);
	ASSERT_SUCCESS(device_read_fifo(config.devices[0], 1000, &count, out));
	ASSERT_EQ(count == 0);
}

TEST(ConfigFifoQueueErrors)
{
	EXPECT_ERROR("(device (fifo))", "expected (fifo ADDRESS (values");
	EXPECT_ERROR("(device (fifo 0 1))", "expected (values");
	EXPECT_ERROR("(device (fifo 65536 (values)))", "between 0 and 65535");
	EXPECT_ERROR("(device (fifo 0 (vals)))", "expected (values");
	EXPECT_ERROR("(device (fifo 0 (values 65536)))", "between 0 and 65535");
	EXPECT_ERROR("(device (fifo 0 (values \"x\")))", "whole numbers");
	EXPECT_ERROR("(device (fifo 0 (values 1)) (fifo 0 (values 2)))", "already defined");
}

TEST(ConfigBindAcceptsValidIpv4)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (bind \"127.0.0.1\")) (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strcmp(config.bind_address, "127.0.0.1") == 0);
}

TEST(ConfigBindRejectsHostname)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(server (bind \"localhost\")) (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "not an IPv4 address") != NULL);
}

TEST(ConfigBindRejectsIpv6)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(server (bind \"::1\")) (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "not an IPv4 address") != NULL);
}

TEST(ConfigBindRejectsOverlongString)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[256];

	/* CONFIG_BIND_MAX is 64; 63 chars is max content, 64+ is overlong. */
	snprintf(text, sizeof(text),
		 "(server (bind \"%.*s\")) (device (holding 0))",
		 64, "111.111.111.111111.111.111.111111.111.111.111111.111.111.1111111");
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "too long") != NULL);
}

TEST(ConfigIdleTimeoutIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (idle-timeout-ms 5000)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.idle_timeout_ms == 5000);

	ASSERT_SUCCESS(load_text("(server (idle-timeout-ms 0)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.idle_timeout_ms == 0);
}

TEST(ConfigIdleTimeoutRejectsInvalidValues)
{
	EXPECT_ERROR("(server (idle-timeout-ms -1)) (device (holding 0))", "between 0 and 86400000");
	EXPECT_ERROR("(server (idle-timeout-ms 86400001)) (device (holding 0))", "between 0 and 86400000");
	EXPECT_ERROR("(server (idle-timeout-ms nope)) (device (holding 0))", "whole number");
}

TEST(ConfigFrameGapIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (frame-gap-ms 5)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.frame_gap_ms == 5);
}

TEST(ConfigFrameGapRejectsInvalidValues)
{
	EXPECT_ERROR("(server (frame-gap-ms 0)) (device (holding 0))", "between 1 and 60000");
	EXPECT_ERROR("(server (frame-gap-ms 60001)) (device (holding 0))", "between 1 and 60000");
	EXPECT_ERROR("(server (frame-gap-ms nope)) (device (holding 0))", "whole number");
}

TEST(ConfigBaudIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (baud 9600)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.baud == 9600);
}

TEST(ConfigBaudRejectsInvalidValues)
{
	EXPECT_ERROR("(server (baud 0)) (device (holding 0))", "between 1 and 4000000");
	EXPECT_ERROR("(server (baud 4000001)) (device (holding 0))", "between 1 and 4000000");
	EXPECT_ERROR("(server (baud nope)) (device (holding 0))", "whole number");
	EXPECT_ERROR("(server (baud 12345)) (device (holding 0))", "standard termios rate");
}

TEST(ConfigFloat32StaticAcceptsNonFinite)
{
	config_t	config = { 0 };
	char		err[256] = "";
	u16		out[2] = { 0, 0 };
	u32		raw = 0;
	f32		real = 0;

	ASSERT_SUCCESS(load_text("(device (holding 0 (type float32) (static +nan.0))\n"
				 "        (holding 2 (type float32) (static +inf.0))\n"
				 "        (holding 4 (type float32) (static -inf.0)))",
				 &config, err, sizeof(err)));
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 0, 2, out));
	raw = ((u32)out[0] << 16) | out[1];
	memcpy(&real, &raw, sizeof(real));
	ASSERT_EQ(isnan(real));
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 2, 2, out));
	ASSERT_EQ(out[0] == 0x7F80 && out[1] == 0x0000);
	ASSERT_SUCCESS(device_read(config.devices[0], SPACE_HOLDING, 4, 2, out));
	ASSERT_EQ(out[0] == 0xFF80 && out[1] == 0x0000);
}

TEST(ConfigNonFloatStaticRejectsNonFinite)
{
	EXPECT_ERROR("(device (holding 0 (type u16) (static +nan.0)))", "only float32");
	EXPECT_ERROR("(device (holding 0 (type u16) (static +inf.0)))", "only float32");
	EXPECT_ERROR("(device (holding 0 (type s32) (static -inf.0)))", "only float32");
}
