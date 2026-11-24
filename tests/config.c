#include "vtest.h"
#include "build_config.h"
#include "config/config.h"
#include "vlib/macros.h"
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

#define EXPECT_ERROR(text, fragment)							\
	do {										\
		config_t	c = { 0 };						\
		char		e[256] = "";						\
		ASSERT_FAILURE(load_text((text), &c, e, sizeof(e)));			\
		ASSERT_EQ(strstr(e, (fragment)) != NULL);				\
	} while (0)

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
	ASSERT_EQ(config.strict_t15 == false);
	ASSERT_EQ(config.broadcast_execute == false);
	ASSERT_EQ(config.data_bits == 8);
	ASSERT_EQ(config.parity == PARITY_NONE);
	ASSERT_EQ(config.stop_bits == 1);
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

TEST(ConfigFaultExceptionAndCorruptAreParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (fault (kind exception) (code 4)) (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->fault.kind == FAULT_EXCEPTION);
	ASSERT_EQ(config.devices[0]->fault.exception_code == 4);
	ASSERT_EQ(config.devices[0]->fault.percent == 100);

	ASSERT_SUCCESS(load_text("(device (fault (kind corrupt) (percent 25)) (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->fault.kind == FAULT_CORRUPT);
	ASSERT_EQ(config.devices[0]->fault.exception_code == 0);
	ASSERT_EQ(config.devices[0]->fault.percent == 25);
}

TEST(ConfigFaultAcceptsEveryStandardExceptionCode)
{
	static const u8 codes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 11 };
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[128];

	for (u32 i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
	{
		snprintf(text, sizeof(text), "(device (fault (kind exception) (code %u)) (holding 0))",
			 codes[i]);
		ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
		ASSERT_EQ(config.devices[0]->fault.exception_code == codes[i]);
	}
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

TEST(ConfigServerIdAdditionalDataIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (server-id-additional-data \"00fFa510\") (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == 4);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[0] == 0x00);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[1] == 0xFF);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[2] == 0xA5);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[3] == 0x10);

	ASSERT_SUCCESS(load_text("(device (server-id-additional-data \"\") (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == 0);

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == 0);
}

TEST(ConfigServerIdAdditionalDataAcceptsMaxLength)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		hex[2 * DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX + 1];
	char		text[sizeof(hex) + 64];

	for (u32 i = 0; i < 2 * DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX; i++)
		hex[i] = (i % 2 == 0) ? 'A' : (char)('0' + (i / 2) % 10);
	hex[2 * DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX] = '\0';
	snprintf(text, sizeof(text), "(device (server-id-additional-data \"%s\"))", hex);

	ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->server_id_additional_data_len == DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[0] == 0xA0);
	ASSERT_EQ(config.devices[0]->server_id_additional_data[DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX - 1] == 0xA8);
}

TEST(ConfigServerIdAdditionalDataRejectsInvalidHex)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(device (server-id-additional-data \"abc\"))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "even number of hex digits") != NULL);

	ASSERT_FAILURE(load_text("(device (server-id-additional-data \"00gg\"))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "invalid hex digit") != NULL);

	ASSERT_FAILURE(load_text("(device (server-id-additional-data 1234))", &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "expected text in double quotes") != NULL);
}

TEST(ConfigServerIdAdditionalDataRejectsOverLength)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		hex[2 * (DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX + 1) + 1];
	char		text[sizeof(hex) + 64];

	memset(hex, '0', sizeof(hex) - 1);
	hex[sizeof(hex) - 1] = '\0';
	snprintf(text, sizeof(text), "(device (server-id-additional-data \"%s\"))", hex);

	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "too many hex digits") != NULL);
	ASSERT_EQ(strstr(err, "249 bytes") != NULL);
}

TEST(ConfigComStatusIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (com-status 65535) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->com_status == 0xFFFF);

	ASSERT_SUCCESS(load_text("(device (com-status 0) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->com_status == 0);

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->com_status == 0);
}

TEST(ConfigComStatusRejectsOtherValues)
{
	EXPECT_ERROR("(device (com-status 1) (holding 0))", "must be 0 (ready) or 65535 (busy)");
	EXPECT_ERROR("(device (com-status 65534) (holding 0))", "must be 0 (ready) or 65535 (busy)");
	EXPECT_ERROR("(device (com-status -1) (holding 0))", "between 0 and 65535");
	EXPECT_ERROR("(device (com-status 65536) (holding 0))", "between 0 and 65535");
	EXPECT_ERROR("(device (com-status \"busy\") (holding 0))", "whole number");
}

/* Builds (device (holding 0) (com-event 0) ... ) with `count` event entries. */
static void
make_com_event_config(char* buf, u32 size, u32 count)
{
	u32	used = (u32)snprintf(buf, size, "(device (holding 0)");

	for (u32 i = 0; i < count; i++)
		used += (u32)snprintf(buf + used, size - used, " (com-event %u)", i);
	snprintf(buf + used, size - used, ")");
}

TEST(ConfigComEventsAreParsedInOrder)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (com-event 0) (com-event 255) (com-event 7) (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->com_event_log_count == 3);
	ASSERT_EQ(config.devices[0]->com_events[0] == 0);
	ASSERT_EQ(config.devices[0]->com_events[1] == 255);
	ASSERT_EQ(config.devices[0]->com_events[2] == 7);

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->com_event_log_count == 0);
}

TEST(ConfigComEventsAcceptMaximum)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[1024];

	make_com_event_config(text, sizeof(text), DEVICE_MAX_COM_EVENTS);
	ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->com_event_log_count == DEVICE_MAX_COM_EVENTS);
	for (u32 i = 0; i < DEVICE_MAX_COM_EVENTS; i++)
		ASSERT_EQ(config.devices[0]->com_events[i] == (u8)i);
}

TEST(ConfigComEventsRejectOverMaxAndInvalidValues)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[1024];

	make_com_event_config(text, sizeof(text), DEVICE_MAX_COM_EVENTS + 1);
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "too many (com-event ...) entries (max 64)") != NULL);

	EXPECT_ERROR("(device (com-event 256) (holding 0))", "between 0 and 255");
	EXPECT_ERROR("(device (com-event -1) (holding 0))", "between 0 and 255");
	EXPECT_ERROR("(device (com-event \"x\") (holding 0))", "whole number");
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

TEST(ConfigDeviceIdObjectDefaultsAndAscendingOrder)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->extended_id_count == 0);

	ASSERT_SUCCESS(load_text(
		"(device (device-id-object 200 \"last\") (device-id-object 128 \"first\")"
		"        (device-id-object 150 \"middle\") (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->extended_id_count == 3);
	ASSERT_EQ(config.devices[0]->extended_id[0].id == 128);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[0].value, "first") == 0);
	ASSERT_EQ(config.devices[0]->extended_id[1].id == 150);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[1].value, "middle") == 0);
	ASSERT_EQ(config.devices[0]->extended_id[2].id == 200);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[2].value, "last") == 0);
}

TEST(ConfigDeviceIdObjectMaxCount)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[2048];
	u32		off = 0;
	char		buf[80];

	off += (u32)snprintf(text + off, sizeof(text) - off, "(device");
	for (u32 i = 0; i < DEVICE_MAX_EXTENDED_IDS; i++)
		off += (u32)snprintf(text + off, sizeof(text) - off,
				    " (device-id-object %u \"v%u\")", 128 + i, i);
	off += (u32)snprintf(text + off, sizeof(text) - off, " (holding 0))");
	ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(config.devices[0]->extended_id_count == DEVICE_MAX_EXTENDED_IDS);
	ASSERT_EQ(config.devices[0]->extended_id[0].id == 128);
	ASSERT_EQ(config.devices[0]->extended_id[DEVICE_MAX_EXTENDED_IDS - 1].id == 159);
	snprintf(buf, sizeof(buf), "v%d", DEVICE_MAX_EXTENDED_IDS - 1);
	ASSERT_EQ(strcmp(config.devices[0]->extended_id[DEVICE_MAX_EXTENDED_IDS - 1].value, buf) == 0);

	config = (config_t){ 0 };
	off = 0;
	off += (u32)snprintf(text + off, sizeof(text) - off, "(device");
	for (u32 i = 0; i <= DEVICE_MAX_EXTENDED_IDS; i++)
		off += (u32)snprintf(text + off, sizeof(text) - off,
				    " (device-id-object %u \"v%u\")", 128 + i, i);
	off += (u32)snprintf(text + off, sizeof(text) - off, " (holding 0))");
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "too many device-id-object entries (max 32)") != NULL);
}

TEST(ConfigDeviceIdObjectRejectsInvalidIds)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(device (device-id-object 127 \"x\") (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "device-id-object ID must be between 128 and 255") != NULL);

	ASSERT_FAILURE(load_text("(device (device-id-object 256 \"x\") (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "device-id-object ID must be between 128 and 255") != NULL);
}

TEST(ConfigDeviceIdObjectRejectsDuplicateIds)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text(
		"(device (device-id-object 200 \"a\") (device-id-object 200 \"b\") (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "device-id-object 200 is configured more than once") != NULL);
}

TEST(ConfigDeviceIdObjectRejectsEmptyAndOverlongText)
{
	config_t	config = { 0 };
	char		err[512] = "";
	char		text[512];
	char		longv[246];

	ASSERT_FAILURE(load_text("(device (device-id-object 128 \"\") (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "device-id-object text must not be empty") != NULL);

	memset(longv, 'x', 245);
	longv[245] = '\0';
	snprintf(text, sizeof(text), "(device (device-id-object 128 \"%s\") (holding 0))", longv);
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "device-id-object text is too long") != NULL);
}

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

TEST(ConfigFaultValidationErrorsAreReadable)
{
	EXPECT_ERROR("(device (fault (kind exception)) (holding 0))", "missing required option (code ...)");
	EXPECT_ERROR("(device (fault (kind exception) (code 9)) (holding 0))", "code 9 is reserved");
	EXPECT_ERROR("(device (fault (kind exception) (code 12)) (holding 0))", "must be between 1 and 11");
	EXPECT_ERROR("(device (fault (kind exception) (code 0)) (holding 0))", "must be between 1 and 11");
	EXPECT_ERROR("(device (fault (kind exception) (code \"4\")) (holding 0))", "expected a whole number");
	EXPECT_ERROR("(device (fault (kind drop) (code 4)) (holding 0))", "code is only valid for kind exception");
	EXPECT_ERROR("(device (fault (kind corrupt) (code 4)) (holding 0))", "code is only valid for kind exception");
	EXPECT_ERROR("(device (fault (kind drop) (delay-ms 100)) (holding 0))", "delay-ms is only valid for kind delay");
	EXPECT_ERROR("(device (fault (kind exception) (code 4) (delay-ms 100)) (holding 0))", "delay-ms is only valid for kind delay");
	EXPECT_ERROR("(device (fault (kind corrupt) (delay-ms 100)) (holding 0))", "delay-ms is only valid for kind delay");
	EXPECT_ERROR("(device (fault (kind exception) (code 4) (code 5)) (holding 0))", "given more than once");
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

	ASSERT_SUCCESS(load_text("(server (mode tcp) (bind \"127.0.0.1\")) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strcmp(config.bind_address, "127.0.0.1") == 0);
}

TEST(ConfigBindRejectsHostname)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_FAILURE(load_text("(server (mode tcp) (bind \"localhost\")) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "not a valid IPv4 or IPv6 address") != NULL);
}

TEST(ConfigBindAcceptsIpv6)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode tcp) (bind \"::1\")) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strcmp(config.bind_address, "::1") == 0);

	ASSERT_SUCCESS(load_text("(server (mode rtu-tcp) (bind \"::\")) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(strcmp(config.bind_address, "::") == 0);
}

TEST(ConfigBindRejectsOverlongString)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[320];
	char		bind[CONFIG_BIND_MAX + 8];

	memset(bind, 'a', CONFIG_BIND_MAX);
	bind[CONFIG_BIND_MAX] = '\0';
	snprintf(text, sizeof(text),
		 "(server (mode tcp) (bind \"%s\")) (device (holding 0))",
		 bind);
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "too long") != NULL);
}

TEST(ConfigIdleTimeoutIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode tcp) (idle-timeout-ms 5000)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.idle_timeout_ms == 5000);

	ASSERT_SUCCESS(load_text("(server (mode rtu-tcp) (idle-timeout-ms 0)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.idle_timeout_ms == 0);
}

TEST(ConfigIdleTimeoutRejectsInvalidValues)
{
	EXPECT_ERROR("(server (mode tcp) (idle-timeout-ms -1)) (device (holding 0))", "between 0 and 86400000");
	EXPECT_ERROR("(server (mode rtu-tcp) (idle-timeout-ms 86400001)) (device (holding 0))", "between 0 and 86400000");
	EXPECT_ERROR("(server (mode tcp) (idle-timeout-ms nope)) (device (holding 0))", "whole number");
}

TEST(ConfigFrameGapIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode rtu-pty) (frame-gap-ms 5)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.frame_gap_ms == 5);
}

TEST(ConfigFrameGapRejectsInvalidValues)
{
	EXPECT_ERROR("(server (mode rtu-pty) (frame-gap-ms 0)) (device (holding 0))", "between 1 and 60000");
	EXPECT_ERROR("(server (mode rtu-tcp) (frame-gap-ms 60001)) (device (holding 0))", "between 1 and 60000");
	EXPECT_ERROR("(server (mode rtu-pty) (frame-gap-ms nope)) (device (holding 0))", "whole number");
}

TEST(ConfigBaudIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode ascii-pty) (baud 9600)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.baud == 9600);
}

TEST(ConfigBaudRejectsInvalidValues)
{
	EXPECT_ERROR("(server (mode rtu-pty) (baud 0)) (device (holding 0))", "between 1 and 4000000");
	EXPECT_ERROR("(server (mode ascii-pty) (baud 4000001)) (device (holding 0))", "between 1 and 4000000");
	EXPECT_ERROR("(server (mode rtu-pty) (baud nope)) (device (holding 0))", "whole number");
	EXPECT_ERROR("(server (mode rtu-tcp) (baud 12345)) (device (holding 0))", "standard termios rate");
}

TEST(ConfigBroadcastExecuteIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode rtu-pty) (broadcast-execute on)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.broadcast_execute == true);

	ASSERT_SUCCESS(load_text("(server (mode rtu-tcp) (broadcast-execute off)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.broadcast_execute == false);

	ASSERT_SUCCESS(load_text("(server (mode rtu-device) (path \"/dev/ttyS0\") (broadcast-execute yes))"
				 " (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.broadcast_execute == true);

	ASSERT_SUCCESS(load_text("(server (mode rtu-pty) (broadcast-execute #f)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.broadcast_execute == false);
}

TEST(ConfigBroadcastExecuteRejectsInvalidValues)
{
	EXPECT_ERROR("(server (mode rtu-pty) (broadcast-execute maybe)) (device (holding 0))", "expected on or off");
	EXPECT_ERROR("(server (mode rtu-tcp) (broadcast-execute 2)) (device (holding 0))", "expected on or off");
}

TEST(ConfigDataBitsAreParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode ascii-pty) (data-bits 7)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.data_bits == 7);

	ASSERT_SUCCESS(load_text("(server (mode ascii-device) (path \"/dev/ttyS0\") (data-bits 8))"
				 " (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.data_bits == 8);
}

TEST(ConfigDataBitsRejectsInvalidValuesAndModes)
{
	EXPECT_ERROR("(server (mode ascii-pty) (data-bits 0)) (device (holding 0))",
		     "between 7 and 8");
	EXPECT_ERROR("(server (mode ascii-pty) (data-bits 9)) (device (holding 0))",
		     "between 7 and 8");
	EXPECT_ERROR("(server (mode ascii-device) (path \"/dev/ttyS0\") (data-bits nope))"
		     " (device (holding 0))", "whole number");
	EXPECT_ERROR("(server (mode rtu-pty) (data-bits 7)) (device (holding 0))",
		     "data-bits is not valid in rtu-pty mode");
	EXPECT_ERROR("(server (mode tcp) (data-bits 8)) (device (holding 0))",
		     "data-bits is not valid in tcp mode");
	EXPECT_ERROR("(server (mode rtu-tcp) (data-bits 7)) (device (holding 0))",
		     "data-bits is not valid in rtu-tcp mode");
}

TEST(ConfigParityIsParsed)
{
	config_t		config = { 0 };
	char			err[256] = "";
	static const char* const names[] = { "none", "even", "odd", "mark", "space" };
	char			text[128];

	for (u32 i = 0; i < ARRAY_COUNT(names); i++)
	{
		snprintf(text, sizeof(text), "(server (mode rtu-pty) (parity %s)) (device (holding 0))", names[i]);
		ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
		ASSERT_EQ(config.parity == (parity_t)i);
	}
}

TEST(ConfigParityRejectsInvalidValues)
{
	EXPECT_ERROR("(server (mode rtu-pty) (parity nope)) (device (holding 0))", "expected one of: none, even, odd, mark, space");
	EXPECT_ERROR("(server (mode rtu-device) (path \"/dev/ttyS0\") (parity 1)) (device (holding 0))", "expected one of: none, even, odd, mark, space");
}

TEST(ConfigStopBitsAreParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode rtu-pty) (stop-bits 1)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.stop_bits == 1);

	ASSERT_SUCCESS(load_text("(server (mode rtu-device) (path \"/dev/ttyS0\") (stop-bits 2))"
				 " (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.stop_bits == 2);
}

TEST(ConfigStopBitsRejectsInvalidValues)
{
	EXPECT_ERROR("(server (mode rtu-pty) (stop-bits 0)) (device (holding 0))", "between 1 and 2");
	EXPECT_ERROR("(server (mode rtu-device) (path \"/dev/ttyS0\") (stop-bits 3)) (device (holding 0))", "between 1 and 2");
	EXPECT_ERROR("(server (mode rtu-pty) (stop-bits nope)) (device (holding 0))", "whole number");
}

TEST(ConfigServerModeNamesAreParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		text[256];

	for (u32 i = 0; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		snprintf(text, sizeof(text), "(server (mode %s)%s) (device (holding 0))",
			 SERVER_MODES[i].name, SERVER_MODES[i].path_option);
		ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
		ASSERT_EQ(config.mode == SERVER_MODES[i].mode);
		if (*SERVER_MODES[i].path_option)
			ASSERT_EQ(strcmp(config.path, "/dev/ttyS0") == 0);
		else
			ASSERT_EQ(config.path[0] == '\0');
	}
}

TEST(ConfigServerModeDefaultsToTcp)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.mode == SERVER_MODE_TCP);
	ASSERT_EQ(config.path[0] == '\0');
}

TEST(ConfigServerModeRejectsUnknownNames)
{
	EXPECT_ERROR("(server (mode rtu)) (device (holding 0))", "expected one of: tcp, rtu-pty, rtu-device, rtu-tcp, ascii-pty, ascii-device, ascii-tcp");
	EXPECT_ERROR("(server (mode \"tcp\")) (device (holding 0))", "expected one of: tcp, rtu-pty, rtu-device, rtu-tcp, ascii-pty, ascii-device, ascii-tcp");
}

TEST(ConfigServerPathIsParsedForDeviceModes)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode rtu-device) (path \"/dev/ttyUSB0\")) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.mode == SERVER_MODE_RTU_DEVICE);
	ASSERT_EQ(strcmp(config.path, "/dev/ttyUSB0") == 0);

	ASSERT_SUCCESS(load_text("(server (mode ascii-device) (path \"/dev/ttyUSB1\")) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.mode == SERVER_MODE_ASCII_DEVICE);
	ASSERT_EQ(strcmp(config.path, "/dev/ttyUSB1") == 0);
}

TEST(ConfigServerPathIsRequiredForDeviceModes)
{
	EXPECT_ERROR("(server (mode rtu-device)) (device (holding 0))", "(path ...) is required");
	EXPECT_ERROR("(server (mode ascii-device)) (device (holding 0))", "(path ...) is required");
}

TEST(ConfigServerPathIsForbiddenForOtherModes)
{
	EXPECT_ERROR("(server (mode tcp) (path \"/dev/ttyS0\")) (device (holding 0))", "(path ...) is only valid");
	EXPECT_ERROR("(server (mode rtu-pty) (path \"/dev/ttyS0\")) (device (holding 0))", "(path ...) is only valid");
	EXPECT_ERROR("(server (mode rtu-tcp) (path \"/dev/ttyS0\")) (device (holding 0))", "(path ...) is only valid");
	EXPECT_ERROR("(server (mode ascii-pty) (path \"/dev/ttyS0\")) (device (holding 0))", "(path ...) is only valid");
	EXPECT_ERROR("(server (mode ascii-tcp) (path \"/dev/ttyS0\")) (device (holding 0))", "(path ...) is only valid");
	EXPECT_ERROR("(server (path \"/dev/ttyS0\")) (device (holding 0))", "(path ...) is only valid");
}

TEST(ConfigServerPathLengthIsValidated)
{
	config_t	config = { 0 };
	char		err[256] = "";
	char		path[CONFIG_PATH_MAX + 1];
	char		text[CONFIG_PATH_MAX + 128];

	memset(path, 'x', CONFIG_PATH_MAX - 1);
	path[CONFIG_PATH_MAX - 1] = '\0';
	snprintf(text, sizeof(text), "(server (mode rtu-device) (path \"%s\")) (device (holding 0))", path);
	ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strlen(config.path) == CONFIG_PATH_MAX - 1);

	memset(path, 'x', CONFIG_PATH_MAX);
	path[CONFIG_PATH_MAX] = '\0';
	snprintf(text, sizeof(text), "(server (mode rtu-device) (path \"%s\")) (device (holding 0))", path);
	ASSERT_FAILURE(load_text(text, &config, err, sizeof(err)));
	ASSERT_EQ(strstr(err, "path is too long") != NULL);
	ASSERT_EQ(strstr(err, "255 characters") != NULL);
}

#if WITH_TLS
TEST(ConfigTlsFullFormIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(server (mode tcp)\n"
		"  (tls (certificate \"/tmp/server.pem\")\n"
		"       (private-key \"/tmp/server.key\")\n"
		"       (ca-certificate \"/tmp/ca.pem\")\n"
		"       (handshake-timeout-ms 250)\n"
		"       (require-client-certificate off)\n"
		"       (min-version tls1.3)\n"
		"       (tls1-2-ciphers \"ECDHE-RSA-AES128-GCM-SHA256\")\n"
		"       (tls1-3-ciphersuites \"TLS_AES_128_GCM_SHA256\")))\n"
		"(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.mode == SERVER_MODE_TCP);
	ASSERT_EQ(config.port == CONFIG_DEFAULT_TLS_PORT);
	ASSERT_EQ(config.tls_enabled);
	ASSERT_EQ(strcmp(config.tls_certificate, "/tmp/server.pem") == 0);
	ASSERT_EQ(strcmp(config.tls_private_key, "/tmp/server.key") == 0);
	ASSERT_EQ(strcmp(config.tls_ca_certificate, "/tmp/ca.pem") == 0);
	ASSERT_EQ(config.tls_require_client_certificate == false);
	ASSERT_EQ(config.tls_min_version == CONFIG_TLS_VERSION_1_3);
	ASSERT_EQ(config.tls_handshake_timeout_ms == 250);
	ASSERT_EQ(strcmp(config.tls1_2_ciphers, "ECDHE-RSA-AES128-GCM-SHA256") == 0);
	ASSERT_EQ(strcmp(config.tls1_3_ciphersuites, "TLS_AES_128_GCM_SHA256") == 0);
}

TEST(ConfigTlsDefaultsAndExplicitPort)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(server (tls (certificate \"cert.pem\") (private-key \"key.pem\")\n"
		"             (ca-certificate \"ca.pem\")))\n"
		"(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.port == CONFIG_DEFAULT_TLS_PORT);
	ASSERT_EQ(config.tls_enabled);
	ASSERT_EQ(config.tls_require_client_certificate == true);
	ASSERT_EQ(config.tls_min_version == CONFIG_TLS_VERSION_1_2);
	ASSERT_EQ(config.tls_handshake_timeout_ms == CONFIG_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS);
	ASSERT_EQ(config.tls1_2_ciphers[0] == '\0');
	ASSERT_EQ(config.tls1_3_ciphersuites[0] == '\0');

	ASSERT_SUCCESS(load_text(
		"(server (mode tcp) (port 1802)\n"
		"        (tls (certificate \"cert.pem\") (private-key \"key.pem\")\n"
		"             (require-client-certificate off)))\n"
		"(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.port == 1802);
	ASSERT_EQ(config.tls_ca_certificate[0] == '\0');
	ASSERT_EQ(config.tls_require_client_certificate == false);
}

TEST(ConfigTlsHandshakeTimeoutIsParsed)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		"             (ca-certificate \"ca.pem\") "
		"             (handshake-timeout-ms 250))) (device (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(config.tls_handshake_timeout_ms == 250);

	ASSERT_SUCCESS(load_text(
		"(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		"             (ca-certificate \"ca.pem\") "
		"             (handshake-timeout-ms 1))) (device (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(config.tls_handshake_timeout_ms == 1);

	ASSERT_SUCCESS(load_text(
		"(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		"             (ca-certificate \"ca.pem\") "
		"             (handshake-timeout-ms 86400000))) (device (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(config.tls_handshake_timeout_ms == 86400000);
}

TEST(ConfigTlsHandshakeTimeoutRejectsInvalidValues)
{
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "         (ca-certificate \"ca.pem\") "
		     "         (handshake-timeout-ms 0))) (device (holding 0))",
		     "between 1 and 86400000");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "         (ca-certificate \"ca.pem\") "
		     "         (handshake-timeout-ms 86400001))) (device (holding 0))",
		     "between 1 and 86400000");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "         (ca-certificate \"ca.pem\") "
		     "         (handshake-timeout-ms nope))) (device (holding 0))",
		     "whole number");
	EXPECT_ERROR("(server (handshake-timeout-ms 250) "
		     "        (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "        (ca-certificate \"ca.pem\"))) "
		     "(device (holding 0))", "unknown option 'handshake-timeout-ms'");
}
#endif

TEST(ConfigPlaintextTcpPortIsUnchanged)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode tcp)) (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.port == CONFIG_DEFAULT_PORT);
	ASSERT_EQ(config.tls_enabled == false);
}

#if WITH_TLS
TEST(ConfigTlsRequiresCertificateAndPrivateKey)
{
	EXPECT_ERROR("(server (tls (private-key \"key.pem\") (ca-certificate \"ca.pem\"))) "
		     "(device (holding 0))", "missing required option (certificate ...)");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (ca-certificate \"ca.pem\"))) "
		     "(device (holding 0))", "missing required option (private-key ...)");
	EXPECT_ERROR("(server (tls (certificate \"\") (private-key \"key.pem\"))) "
		     "(device (holding 0))", "certificate path must not be empty");
}

TEST(ConfigTlsRequiresCaForClientAuthentication)
{
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\"))) "
		     "(device (holding 0))", "ca-certificate is required");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(require-client-certificate on))) (device (holding 0))",
		     "ca-certificate is required");
}

TEST(ConfigTlsValidatesMinVersionAndBooleans)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		"(require-client-certificate off) (min-version tls1.2))) "
		"(device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.tls_min_version == CONFIG_TLS_VERSION_1_2);

	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(min-version tls1.1))) (device (holding 0))",
		     "expected one of: tls1.2, tls1.3");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(min-version \"tls1.3\"))) (device (holding 0))",
		     "expected one of: tls1.2, tls1.3");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(require-client-certificate maybe))) (device (holding 0))",
		     "expected on or off");
}

TEST(ConfigTlsValidatesCipherStrings)
{
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(tls1-2-ciphers \"\"))) (device (holding 0))", "tls1-2-ciphers must not be empty");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(tls1-3-ciphersuites \"\"))) (device (holding 0))",
		     "tls1-3-ciphersuites must not be empty");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(tls1-2-ciphers 7))) (device (holding 0))", "expected text in double quotes");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(tls1-3-ciphersuites 7))) (device (holding 0))", "expected text in double quotes");
}

TEST(ConfigTlsValidatesNestedKeysDuplicatesAndPathLength)
{
	char	path[CONFIG_TLS_PATH_MAX + 2];
	char	text[CONFIG_TLS_PATH_MAX + 160];

	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (private-key \"key.pem\") "
		     "(unknown 1))) (device (holding 0))", "unknown option 'unknown'");
	EXPECT_ERROR("(server (tls (certificate \"cert.pem\") (certificate \"other.pem\") "
		     "(private-key \"key.pem\"))) (device (holding 0))",
		     "option 'certificate' is given more than once");

	memset(path, 'x', CONFIG_TLS_PATH_MAX);
	path[CONFIG_TLS_PATH_MAX] = '\0';
	snprintf(text, sizeof(text),
		 "(server (tls (certificate \"%s\") (private-key \"key.pem\"))) "
		 "(device (holding 0))", path);
	EXPECT_ERROR(text, "certificate path is too long");
}

TEST(ConfigTlsIsRejectedOutsideTcpMode)
{
	char	text[256];

	for (u32 i = 1; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		snprintf(text, sizeof(text), "(server (mode %s)%s (tls)) (device (holding 0))",
			 SERVER_MODES[i].name, SERVER_MODES[i].path_option);
		EXPECT_ERROR(text, "not valid in");
	}
}
#else
TEST(ConfigTlsRejectedWithoutTlsSupport)
{
	EXPECT_ERROR("(server (mode tcp) (tls (certificate \"cert.pem\") "
		     "(private-key \"key.pem\") (ca-certificate \"ca.pem\"))) "
		     "(device (holding 0))", "built without TLS support");
	EXPECT_ERROR("(server (mode rtu-pty) (tls)) (device (holding 0))",
		     "built without TLS support");
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
	char			err[256] = "";
	char			text[256];
	static const u32	ids[] = { 1, 247 };

	for (u32 i = 1; i < ARRAY_COUNT(SERVER_MODES); i++)
	{
		for (u32 j = 0; j < ARRAY_COUNT(ids); j++)
		{
			snprintf(text, sizeof(text), "(server (mode %s)%s) (device (unit-id %u) (holding 0))",
				 SERVER_MODES[i].name, SERVER_MODES[i].path_option, ids[j]);
			ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
			ASSERT_EQ(config.devices[0]->unit_id == ids[j]);
		}
	}
}

TEST(ConfigTcpModeAcceptsFullUnitIdRange)
{
	config_t		config = { 0 };
	char			err[256] = "";
	char			text[192];
	static const u32	ids[] = { 0, 255 };

	for (u32 i = 0; i < ARRAY_COUNT(ids); i++)
	{
		snprintf(text, sizeof(text), "(server (mode tcp)) (device (unit-id %u) (holding 0))", ids[i]);
		ASSERT_SUCCESS(load_text(text, &config, err, sizeof(err)));
		ASSERT_EQ(config.mode == SERVER_MODE_TCP);
		ASSERT_EQ(config.devices[0]->unit_id == ids[i]);
	}
}

TEST(ConfigRtuTcpRejectsSerialOptions)
{
	/* Presence alone is invalid here: even the default value is rejected. */
	EXPECT_ERROR("(server (mode rtu-tcp) (strict-t15 on)) (device (holding 0))",
		     "strict-t15 is not valid in rtu-tcp mode");
	EXPECT_ERROR("(server (mode rtu-tcp) (strict-t15 off)) (device (holding 0))",
		     "strict-t15 is not valid in rtu-tcp mode");
	EXPECT_ERROR("(server (mode rtu-tcp) (parity none)) (device (holding 0))",
		     "parity is not valid in rtu-tcp mode");
	EXPECT_ERROR("(server (mode rtu-tcp) (parity even)) (device (holding 0))",
		     "parity is not valid in rtu-tcp mode");
	EXPECT_ERROR("(server (mode rtu-tcp) (stop-bits 1)) (device (holding 0))",
		     "stop-bits is not valid in rtu-tcp mode");
	EXPECT_ERROR("(server (mode rtu-tcp) (stop-bits 2)) (device (holding 0))",
		     "stop-bits is not valid in rtu-tcp mode");
}

TEST(ConfigRtuTcpAcceptsListenAndTimingOptions)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(server (mode rtu-tcp) (port 1503) (bind \"127.0.0.1\") (seed 4)"
		" (idle-timeout-ms 500) (baud 9600) (frame-gap-ms 20) (broadcast-execute on))"
		" (device (unit-id 1) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.port == 1503);
	ASSERT_EQ(strcmp(config.bind_address, "127.0.0.1") == 0);
	ASSERT_EQ(config.seed == 4);
	ASSERT_EQ(config.idle_timeout_ms == 500);
	ASSERT_EQ(config.baud == 9600);
	ASSERT_EQ(config.frame_gap_ms == 20);
	ASSERT_EQ(config.broadcast_execute == true);
}

TEST(ConfigTcpModeRejectsSerialOptions)
{
	EXPECT_ERROR("(server (mode tcp) (broadcast-execute on)) (device (holding 0))",
		     "broadcast-execute is not valid in tcp mode");
	EXPECT_ERROR("(server (broadcast-execute off)) (device (holding 0))",
		     "broadcast-execute is not valid in tcp mode");
	EXPECT_ERROR("(server (mode tcp) (baud 9600)) (device (holding 0))",
		     "baud is not valid in tcp mode");
	EXPECT_ERROR("(server (mode tcp) (frame-gap-ms 20)) (device (holding 0))",
		     "frame-gap-ms is not valid in tcp mode");
	EXPECT_ERROR("(server (mode tcp) (strict-t15 off)) (device (holding 0))",
		     "strict-t15 is not valid in tcp mode");
	EXPECT_ERROR("(server (mode tcp) (parity none)) (device (holding 0))",
		     "parity is not valid in tcp mode");
	EXPECT_ERROR("(server (mode tcp) (stop-bits 1)) (device (holding 0))",
		     "stop-bits is not valid in tcp mode");
	EXPECT_ERROR("(server (mode tcp) (data-bits 7)) (device (holding 0))",
		     "data-bits is not valid in tcp mode");
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
			snprintf(text, sizeof(text), "(server (mode %s)%s %s) (device (holding 0))",
				 SERVER_MODES[i].name, SERVER_MODES[i].path_option, options[j]);
			snprintf(fragment, sizeof(fragment), "is not valid in %s mode", SERVER_MODES[i].name);
			EXPECT_ERROR(text, fragment);
		}
	}
}

TEST(ConfigAsciiModeRejectsRtuOptionsAndBroadcast)
{
	EXPECT_ERROR("(server (mode ascii-pty) (broadcast-execute on)) (device (holding 0))",
		     "broadcast-execute is not valid in ascii-pty mode (ASCII never executes unit-0 broadcasts)");
	EXPECT_ERROR("(server (mode ascii-device) (path \"/dev/ttyS0\") (broadcast-execute off))"
		     " (device (holding 0))",
		     "broadcast-execute is not valid in ascii-device mode (ASCII never executes unit-0 broadcasts)");
	EXPECT_ERROR("(server (mode ascii-pty) (frame-gap-ms 20)) (device (holding 0))",
		     "frame-gap-ms is not valid in ascii-pty mode");
	EXPECT_ERROR("(server (mode ascii-pty) (strict-t15 on)) (device (holding 0))",
		     "strict-t15 is not valid in ascii-pty mode");
}

TEST(ConfigAsciiModeAcceptsLineSettings)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(server (mode ascii-pty) (seed 3) (baud 115200)"
		" (data-bits 7) (parity even) (stop-bits 1)) (device (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(config.baud == 115200);
	ASSERT_EQ(config.seed == 3);
	ASSERT_EQ(config.data_bits == 7);
	ASSERT_EQ(config.parity == PARITY_EVEN);
	ASSERT_EQ(config.stop_bits == 1);

	ASSERT_SUCCESS(load_text(
		"(server (mode ascii-device) (path \"/dev/ttyS0\") (data-bits 8)"
		" (parity odd) (stop-bits 2)) (device (holding 0))",
		&config, err, sizeof(err)));
	ASSERT_EQ(config.data_bits == 8);
	ASSERT_EQ(config.parity == PARITY_ODD);
	ASSERT_EQ(config.stop_bits == 2);
}

TEST(ConfigAsciiModeAcceptsBaudOnly)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode ascii-pty) (seed 3) (baud 115200)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.baud == 115200);
	ASSERT_EQ(config.seed == 3);
}

TEST(ConfigAsciiTcpAcceptsListenOptions)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text("(server (mode ascii-tcp)) (device (holding 0))",
				 &config, err, sizeof(err)));
	ASSERT_EQ(config.mode == SERVER_MODE_ASCII_TCP);
	ASSERT_EQ(config.port == CONFIG_DEFAULT_PORT);
	ASSERT_EQ(strcmp(config.bind_address, "0.0.0.0") == 0);

	ASSERT_SUCCESS(load_text(
		"(server (mode ascii-tcp) (port 1503) (bind \"::1\") (seed 4) (idle-timeout-ms 500))"
		" (device (unit-id 1) (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.port == 1503);
	ASSERT_EQ(strcmp(config.bind_address, "::1") == 0);
	ASSERT_EQ(config.seed == 4);
	ASSERT_EQ(config.idle_timeout_ms == 500);
}

TEST(ConfigAsciiTcpRejectsSerialOptionsAndBroadcast)
{
	EXPECT_ERROR("(server (mode ascii-tcp) (path \"/dev/ttyS0\")) (device (holding 0))",
		     "(path ...) is only valid");
	EXPECT_ERROR("(server (mode ascii-tcp) (baud 9600)) (device (holding 0))",
		     "baud is not valid in ascii-tcp mode");
	EXPECT_ERROR("(server (mode ascii-tcp) (frame-gap-ms 20)) (device (holding 0))",
		     "frame-gap-ms is not valid in ascii-tcp mode");
	EXPECT_ERROR("(server (mode ascii-tcp) (strict-t15 on)) (device (holding 0))",
		     "strict-t15 is not valid in ascii-tcp mode");
	EXPECT_ERROR("(server (mode ascii-tcp) (broadcast-execute on)) (device (holding 0))",
		     "broadcast-execute is not valid in ascii-tcp mode");
	EXPECT_ERROR("(server (mode ascii-tcp) (broadcast-execute off)) (device (holding 0))",
		     "broadcast-execute is not valid in ascii-tcp mode (ASCII never executes unit-0 broadcasts)");
	EXPECT_ERROR("(server (mode ascii-tcp) (data-bits 7)) (device (holding 0))",
		     "data-bits is not valid in ascii-tcp mode");
	EXPECT_ERROR("(server (mode ascii-tcp) (parity even)) (device (holding 0))",
		     "parity is not valid in ascii-tcp mode");
	EXPECT_ERROR("(server (mode ascii-tcp) (stop-bits 1)) (device (holding 0))",
		     "stop-bits is not valid in ascii-tcp mode");
}

TEST(ConfigSerialModesAcceptSerialOptions)
{
	config_t	config = { 0 };
	char		err[256] = "";

	ASSERT_SUCCESS(load_text(
		"(server (mode rtu-pty) (strict-t15 on) (parity even) (stop-bits 2) (baud 9600))"
		" (device (holding 0))", &config, err, sizeof(err)));
	ASSERT_EQ(config.strict_t15 == true);
	ASSERT_EQ(config.parity == PARITY_EVEN);
	ASSERT_EQ(config.stop_bits == 2);
	ASSERT_EQ(config.baud == 9600);
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
