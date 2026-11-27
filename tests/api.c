/* Public C API (include/mogbus.h), driven in-process the way an embedding host would. */
#define _GNU_SOURCE
#include "vtest.h"
#include "tests/tests_utils/test_case.h"
#include "build_config.h"
#include "mogbus.h"
#include "modbus/ascii.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/rtu_framer.h"
#include "os/os_time.h"
#include "util/rng.h"
#include "tests/tests_utils/test_support.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static const u8 READ_HOLDING[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };

static mogbus_point_t
holding(uint32_t address, double value)
{
	return (mogbus_point_t){
		.space = MOGBUS_HOLDING, .address = address, .type = MOGBUS_TYPE_U16, .value = value,
	};
}

/* Steps the server until fd is readable. */
static bool
pump(mogbus_server_t* server, int fd)
{
	u64 deadline = os_now_ms() + 2000;

	while (os_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		(void)mogbus_server_step(server, 1);
		if (poll(&pfd, 1, 0) > 0)
			return true;
	}
	return false;
}

TEST(ApiVersionAndDefaults)
{
	mogbus_server_options_t o = mogbus_server_options_defaults();

	ASSERT_EQ(strcmp(mogbus_version(), "0.0.1") == 0);
	ASSERT_EQ(o.mode == MOGBUS_MODE_TCP && strcmp(o.bind_address, "127.0.0.1") == 0);
	ASSERT_EQ(o.port == 1502 && o.fd == -1 && o.idle_timeout_ms == 0);
	ASSERT_EQ(o.data_bits == 8 && o.parity == MOGBUS_PARITY_NONE && o.stop_bits == 1);
	ASSERT_EQ(o.tls_require_client_certificate && o.tls_handshake_timeout_ms == 10000);
	ASSERT_EQ(o.show_registers_ms == 0);
	mogbus_destroy(NULL);
}

TEST(ApiShowRegistersMsIsValidated)
{
	mogbus_t*		ctx = NULL;
	mogbus_device_t*	dev;
	mogbus_point_t		p = holding(0, 1);
	mogbus_server_options_t	o = mogbus_server_options_defaults();
	static const uint32_t	values[] = { 0, 1, 9, 10, 1000, 60000, 60001 };

	mogbus_create(&ctx);
	mogbus_add_device(ctx, 1, "d", &dev);
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
	{
		o.show_registers_ms = values[i];
		ASSERT_EQ((mogbus_server_check(ctx, &o) == MOGBUS_OK) ==
			  (values[i] == 0 || (values[i] >= 10 && values[i] <= 60000)));
	}
	o.show_registers_ms = 5;
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_ERR_INVALID &&
		  strstr(mogbus_last_error(ctx), "show_registers_ms") != NULL);
	mogbus_destroy(ctx);
}

TEST(ApiDevicesPointsAndValues)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev;
	mogbus_point_t	p;
	uint16_t	words[2] = { 0, 0 };
	double		v = 0;

	ASSERT_EQ(ctx != NULL);
	mogbus_add_device(ctx, 7, "pump", &dev);
	ASSERT_EQ(dev != NULL && mogbus_find_device(ctx, 7) == dev);
	ASSERT_EQ(mogbus_find_device(ctx, 8) == NULL);

	p = holding(0, 42);
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_INPUT, .address = 10, .type = MOGBUS_TYPE_F32, .value = 1.5,
			      .word_order = MOGBUS_WORDS_LOW_FIRST };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_COIL, .address = 3, .type = MOGBUS_TYPE_BOOL, .value = 1 };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 20, .type = MOGBUS_TYPE_U16,
			      .kind = MOGBUS_POINT_RAMP,
			      .ramp = { .from = 5, .to = 5, .step = 1, .every_ms = 1000, .mode = MOGBUS_RAMP_HOLD } };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);

	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 1, words) == MOGBUS_OK && words[0] == 42);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_INPUT, 10, &v) == MOGBUS_OK && v == 1.5);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_COIL, 3, &v) == MOGBUS_OK && v == 1.0);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 20, &v) == MOGBUS_OK && v == 5.0);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 1, &v) != MOGBUS_OK);	/* undefined */
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 2, words) != MOGBUS_OK);	/* 1 is undefined */

	/* A raw write behaves like a client write; the point stays writable. */
	words[0] = 99;
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 0, 1, words) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) == MOGBUS_OK && v == 99);
	words[0] = 2;
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_COIL, 3, 1, words) != MOGBUS_OK);	/* bits are 0/1 */

	/* set_value claims the point for the host, also on read-only spaces. */
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_INPUT, 10, -2.25) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_INPUT, 10, &v) == MOGBUS_OK && v == -2.25);
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_COIL, 3, 0) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_COIL, 3, &v) == MOGBUS_OK && v == 0.0);
	words[0] = 1;
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_COIL, 3, 1, words) != MOGBUS_OK);	/* host-owned now */
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 5, 1) != MOGBUS_OK);	/* no point there */
	mogbus_destroy(ctx);
}

TEST(ApiReadOnlyPointsRefuseClientsButNotTheHost)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev = NULL;
	mogbus_add_device(ctx, 1, "ro", &dev);
	mogbus_point_t	p = holding(0, 5);
	uint16_t	words[1] = { 77 };
	double		v = 0;

	p.read_only = true;
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_INPUT, .address = 0, .type = MOGBUS_TYPE_U16, .read_only = true };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);

	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 0, 1, words) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) == MOGBUS_OK && v == 77);
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 0, 12) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) == MOGBUS_OK && v == 12);
	mogbus_destroy(ctx);
}

TEST(ApiClockPointsShowTheWallClock)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev = NULL;
	mogbus_add_device(ctx, 1, "clock", &dev);
	mogbus_point_t	p = { .space = MOGBUS_HOLDING, .type = MOGBUS_TYPE_U16, .kind = MOGBUS_POINT_CLOCK,
			      .clock_field = MOGBUS_CLOCK_YEAR, .clock_utc = true };
	double		v = 0;

	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) == MOGBUS_OK && v >= 2024);
	p.address     = 1;
	p.clock_field = MOGBUS_CLOCK_UNIX;
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "32-bit") != NULL);
	mogbus_destroy(ctx);
}

/* ---- access hook ---- */

static struct hook_log_t
{
	int		calls;
	int		reads;
	int		writes;
	mogbus_access_t	last;
	uint16_t	values[8];
	uint8_t		order[4];	/* kinds in call order */
	uint8_t		answer;		/* exception to return */
	double		refresh;	/* READ: set holding 0 to this */
} hook_log;

static uint8_t
test_hook(void* user, mogbus_device_t* dev, const mogbus_access_t* a)
{
	(void)user;
	if (hook_log.calls < 4)
		hook_log.order[hook_log.calls] = (uint8_t)a->kind;
	hook_log.calls++;
	hook_log.last = *a;
	if (a->values)
		memcpy(hook_log.values, a->values, a->count * sizeof(uint16_t) > sizeof(hook_log.values) ?
		       sizeof(hook_log.values) : a->count * sizeof(uint16_t));
	if (a->kind == MOGBUS_ACCESS_READ)
	{
		hook_log.reads++;
		if (!hook_log.answer)
			mogbus_device_set_value(dev, MOGBUS_HOLDING, 0, hook_log.refresh);
	}
	else
		hook_log.writes++;
	return hook_log.answer;
}

static uint32_t
hook_pdu(mogbus_device_t* dev, const u8* req, u32 len, u8* resp)
{
	return modbus_handle_pdu((device_t*)dev, 0, req, len, resp);
}

static mogbus_device_t*
hook_device(mogbus_t** ctx)
{
	mogbus_device_t*	dev = NULL;
	mogbus_point_t		p   = holding(0, 1);

	mogbus_create(ctx);
	mogbus_add_device(*ctx, 1, "hook", &dev);
	for (uint32_t i = 0; i < 4; i++)
	{
		p = holding(i, 1);
		(void)mogbus_device_add_point(dev, &p);
	}
	p = holding(10, 5);
	p.read_only = true;
	(void)mogbus_device_add_point(dev, &p);
	p = (mogbus_point_t){ .space = MOGBUS_COIL, .address = 0, .type = MOGBUS_TYPE_BOOL };
	(void)mogbus_device_add_point(dev, &p);
	memset(&hook_log, 0, sizeof(hook_log));
	ASSERT_EQ(mogbus_device_set_access_hook(dev, test_hook, NULL) == MOGBUS_OK);
	return dev;
}

TEST(ApiAccessHookRefreshesValuesBeforeAClientRead)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 2 };

	hook_log.refresh = 321;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 6);
	ASSERT_EQ(resp[2] == 0x01 && resp[3] == 0x41 && resp[5] == 1);
	ASSERT_EQ(hook_log.calls == 1 && hook_log.last.kind == MOGBUS_ACCESS_READ);
	ASSERT_EQ(hook_log.last.space == MOGBUS_HOLDING && hook_log.last.address == 0);
	ASSERT_EQ(hook_log.last.count == 2 && hook_log.last.values == NULL);
	ASSERT_EQ(hook_log.last.function_code == FC_READ_HOLDING_REGISTERS);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookReadExceptionAnswersIt)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };

	hook_log.answer = 6;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80) && resp[1] == 6);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookWriteVetoChangesNothing)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_WRITE_MULTIPLE_REGISTERS, 0, 0, 0, 3, 6, 0, 7, 0, 8, 0, 9 };
	uint16_t	words[3];

	hook_log.answer = 4;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 2 && resp[1] == 4);
	ASSERT_EQ(hook_log.writes == 1 && hook_log.last.count == 3);
	ASSERT_EQ(hook_log.values[0] == 7 && hook_log.values[1] == 8 && hook_log.values[2] == 9);
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 3, words) == MOGBUS_OK);
	ASSERT_EQ(words[0] == 1 && words[1] == 1 && words[2] == 1);

	hook_log.answer = 0;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 3, words) == MOGBUS_OK);
	ASSERT_EQ(words[0] == 7 && words[1] == 8 && words[2] == 9);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookSeesSingleCoilAndMaskWriteResults)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		coil[] = { FC_WRITE_SINGLE_COIL, 0, 0, 0xFF, 0x00 };
	u8		mask[] = { FC_MASK_WRITE_REGISTER, 0, 1, 0x00, 0xF0, 0x00, 0x0A };
	u8		single[] = { FC_WRITE_SINGLE_REGISTER, 0, 2, 0x12, 0x34 };

	ASSERT_EQ(hook_pdu(dev, coil, sizeof(coil), resp) == 5);
	ASSERT_EQ(hook_log.last.kind == MOGBUS_ACCESS_WRITE && hook_log.last.space == MOGBUS_COIL);
	ASSERT_EQ(hook_log.values[0] == 1 && hook_log.last.function_code == FC_WRITE_SINGLE_COIL);
	/* holding 1 is 1: (1 & 0x00F0) | (0x000A & ~0x00F0) = 0x0A */
	ASSERT_EQ(hook_pdu(dev, mask, sizeof(mask), resp) == 7);
	ASSERT_EQ(hook_log.last.address == 1 && hook_log.last.count == 1 && hook_log.values[0] == 0x0A);
	ASSERT_EQ(hook_log.last.function_code == FC_MASK_WRITE_REGISTER);
	ASSERT_EQ(hook_pdu(dev, single, sizeof(single), resp) == 5);
	ASSERT_EQ(hook_log.values[0] == 0x1234 && hook_log.last.function_code == FC_WRITE_SINGLE_REGISTER);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookReadWriteMultipleWritesFirst)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0, 0, 0, 1, 0, 1, 0, 1, 2, 0, 9 };

	hook_log.refresh = 100;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 4);
	ASSERT_EQ(hook_log.calls == 2 && hook_log.order[0] == MOGBUS_ACCESS_WRITE);
	ASSERT_EQ(hook_log.order[1] == MOGBUS_ACCESS_READ);
	ASSERT_EQ(resp[2] == 0 && resp[3] == 100);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookIsSkippedForBadRangesAndHostCalls)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		undefined_read[] = { FC_READ_HOLDING_REGISTERS, 0x01, 0x00, 0, 1 };
	u8		undefined_write[] = { FC_WRITE_SINGLE_REGISTER, 0x01, 0x00, 0, 1 };
	u8		read_only_write[] = { FC_WRITE_SINGLE_REGISTER, 0, 10, 0, 1 };
	uint16_t	word = 5;

	ASSERT_EQ(hook_pdu(dev, undefined_read, sizeof(undefined_read), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(hook_pdu(dev, undefined_write, sizeof(undefined_write), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(hook_pdu(dev, read_only_write, sizeof(read_only_write), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 0, 1, &word) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 1, 3) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 1, &word) == MOGBUS_OK);
	ASSERT_EQ(hook_log.calls == 0);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookCanBeReplacedAndCleared)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };

	ASSERT_EQ(mogbus_device_set_access_hook(dev, test_hook, NULL) == MOGBUS_OK);	/* replace */
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 4 && hook_log.calls == 1);
	ASSERT_EQ(mogbus_device_set_access_hook(dev, NULL, NULL) == MOGBUS_OK);
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 4 && hook_log.calls == 1);
	ASSERT_EQ(mogbus_device_set_access_hook(NULL, test_hook, NULL) != MOGBUS_OK);
	mogbus_destroy(ctx);
}

static struct written_log_t
{
	int		calls;
	mogbus_access_t	seen[8];
	uint16_t	values[8][8];
	int		reentrant;	/* set holding 3 to calls on every notification */
} written_log;

static void
test_written(void* user, mogbus_device_t* dev, const mogbus_access_t* a)
{
	int	n = written_log.calls++;

	(void)user;
	if (n < 8)
	{
		written_log.seen[n] = *a;
		if (a->values)
			memcpy(written_log.values[n], a->values, (a->count < 8 ? a->count : 8) * sizeof(uint16_t));
	}
	if (written_log.reentrant)
		(void)mogbus_device_set_value(dev, MOGBUS_HOLDING, 3, written_log.calls);
}

TEST(ApiWrittenHookRunsAfterEachClientWriteFunction)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		coil[] = { FC_WRITE_SINGLE_COIL, 0, 0, 0xFF, 0x00 };
	u8		single[] = { FC_WRITE_SINGLE_REGISTER, 0, 2, 0x12, 0x34 };
	u8		multi[] = { FC_WRITE_MULTIPLE_REGISTERS, 0, 0, 0, 2, 4, 0, 7, 0, 8 };
	u8		coils[] = { FC_WRITE_MULTIPLE_COILS, 0, 0, 0, 1, 1, 0 };
	u8		mask[] = { FC_MASK_WRITE_REGISTER, 0, 1, 0x00, 0xF0, 0x00, 0x0A };
	u8		rw[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0, 0, 0, 1, 0, 1, 0, 1, 2, 0, 9 };
	u8		read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	double		v = 0;

	written_log.calls = 0;
	written_log.reentrant = 1;
	ASSERT_EQ(mogbus_device_set_written_hook(dev, test_written, NULL) == MOGBUS_OK);

	ASSERT_EQ(hook_pdu(dev, coil, sizeof(coil), resp) == 5);
	ASSERT_EQ(written_log.calls == 1 && written_log.seen[0].kind == MOGBUS_ACCESS_WRITE);
	ASSERT_EQ(written_log.seen[0].space == MOGBUS_COIL && written_log.seen[0].count == 1);
	ASSERT_EQ(written_log.values[0][0] == 1 && written_log.seen[0].function_code == FC_WRITE_SINGLE_COIL);
	ASSERT_EQ(hook_pdu(dev, single, sizeof(single), resp) == 5);
	ASSERT_EQ(written_log.calls == 2 && written_log.seen[1].space == MOGBUS_HOLDING);
	ASSERT_EQ(written_log.seen[1].address == 2 && written_log.values[1][0] == 0x1234);
	ASSERT_EQ(hook_pdu(dev, multi, sizeof(multi), resp) == 5);
	ASSERT_EQ(written_log.calls == 3 && written_log.seen[2].count == 2);
	ASSERT_EQ(written_log.values[2][0] == 7 && written_log.values[2][1] == 8);
	ASSERT_EQ(written_log.seen[2].function_code == FC_WRITE_MULTIPLE_REGISTERS);
	ASSERT_EQ(hook_pdu(dev, coils, sizeof(coils), resp) == 5);
	ASSERT_EQ(written_log.calls == 4 && written_log.seen[3].space == MOGBUS_COIL);
	ASSERT_EQ(written_log.values[3][0] == 0 && written_log.seen[3].function_code == FC_WRITE_MULTIPLE_COILS);
	/* holding 1 is 1: (1 & 0x00F0) | (0x000A & ~0x00F0) = 0x0A, the stored value */
	ASSERT_EQ(hook_pdu(dev, mask, sizeof(mask), resp) == 7);
	ASSERT_EQ(written_log.calls == 5 && written_log.values[4][0] == 0x0A);
	ASSERT_EQ(written_log.seen[4].function_code == FC_MASK_WRITE_REGISTER);
	ASSERT_EQ(hook_pdu(dev, rw, sizeof(rw), resp) == 4);
	ASSERT_EQ(written_log.calls == 6 && written_log.seen[5].address == 1 && written_log.values[5][0] == 9);
	ASSERT_EQ(written_log.seen[5].function_code == FC_READ_WRITE_MULTIPLE_REGISTERS);

	/* the hook's own host writes did not recurse, and reads are not notified */
	ASSERT_EQ(hook_pdu(dev, read, sizeof(read), resp) == 4 && written_log.calls == 6);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 3, &v) == MOGBUS_OK && v == 6);
	mogbus_destroy(ctx);
}

TEST(ApiWrittenHookRunsOncePerFileRecordSubRequest)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_WRITE_FILE_RECORD, 20,
			  6, 0, 1, 0, 0, 0, 2, 0, 5, 0, 6,	/* file 1, records 0-1 */
			  6, 0, 2, 0, 5, 0, 1, 0, 7 };		/* file 2, record 5 */
	u8		again[] = { FC_WRITE_FILE_RECORD, 9, 6, 0, 1, 0, 0, 0, 1, 0, 5 };
	uint16_t	file1[] = { 1, 2, 3 };
	uint16_t	file2[] = { 4 };
	uint16_t	out[3];

	ASSERT_EQ(mogbus_device_add_file_record(dev, 1, 0, 3, file1) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 2, 5, 1, file2) == MOGBUS_OK);
	written_log.calls = 0;
	written_log.reentrant = 0;
	ASSERT_EQ(mogbus_device_set_written_hook(dev, test_written, NULL) == MOGBUS_OK);

	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(written_log.calls == 2);
	ASSERT_EQ(written_log.seen[0].target == MOGBUS_TARGET_FILE_RECORD && written_log.seen[0].file == 1);
	ASSERT_EQ(written_log.seen[0].address == 0 && written_log.seen[0].count == 2);
	ASSERT_EQ(written_log.values[0][0] == 5 && written_log.values[0][1] == 6);
	ASSERT_EQ(written_log.seen[0].function_code == FC_WRITE_FILE_RECORD);
	ASSERT_EQ(written_log.seen[1].file == 2 && written_log.seen[1].address == 5 && written_log.values[1][0] == 7);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 1, 0, 3, out) == MOGBUS_OK && out[0] == 5 && out[1] == 6 && out[2] == 3);

	/* a veto by the access hook happens before anything is written and never reaches it */
	memset(&hook_log, 0, sizeof(hook_log));
	hook_log.answer = 4;
	ASSERT_EQ(hook_pdu(dev, again, sizeof(again), resp) == 2 && resp[1] == 4);
	ASSERT_EQ(written_log.calls == 2);
	mogbus_destroy(ctx);
}

TEST(ApiWrittenHookIsSkippedForVetoesExceptionsAndHostWrites)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = hook_device(&ctx);
	u8		resp[260];
	u8		write[] = { FC_WRITE_SINGLE_REGISTER, 0, 0, 0, 5 };
	u8		undefined[] = { FC_WRITE_SINGLE_REGISTER, 0x01, 0x00, 0, 1 };
	u8		read_only[] = { FC_WRITE_SINGLE_REGISTER, 0, 10, 0, 1 };
	u8		bad_value[] = { FC_WRITE_SINGLE_COIL, 0, 0, 0x12, 0x34 };
	uint16_t	word = 5;

	written_log.calls = 0;
	written_log.reentrant = 0;
	ASSERT_EQ(mogbus_device_set_written_hook(dev, test_written, NULL) == MOGBUS_OK);

	hook_log.answer = 6;
	ASSERT_EQ(hook_pdu(dev, write, sizeof(write), resp) == 2 && resp[1] == 6);
	hook_log.answer = 0;
	ASSERT_EQ(hook_pdu(dev, undefined, sizeof(undefined), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(hook_pdu(dev, read_only, sizeof(read_only), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(hook_pdu(dev, bad_value, sizeof(bad_value), resp) == 2 && resp[1] == 3);
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 0, 1, &word) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 1, 3) == MOGBUS_OK);
	ASSERT_EQ(written_log.calls == 0);

	ASSERT_EQ(hook_pdu(dev, write, sizeof(write), resp) == 5 && written_log.calls == 1);
	ASSERT_EQ(mogbus_device_set_written_hook(dev, NULL, NULL) == MOGBUS_OK);
	ASSERT_EQ(hook_pdu(dev, write, sizeof(write), resp) == 5 && written_log.calls == 1);
	ASSERT_EQ(mogbus_device_set_written_hook(NULL, test_written, NULL) != MOGBUS_OK);
	mogbus_destroy(ctx);
}

TEST(ApiWrittenHookRunsOncePerDeviceOnBroadcastExecute)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* a = NULL;
	mogbus_add_device(ctx, 1, "a", &a);
	mogbus_device_t* b = NULL;
	mogbus_add_device(ctx, 2, "b", &b);
	mogbus_point_t	p = holding(0, 0);
	device_t*	devices[2] = { (device_t*)a, (device_t*)b };
	rtu_framer_t	framer;
	rng_t		rng;
	rtu_framer_ctx_t fctx = { .devices = devices, .device_count = 2, .rng = &rng, .broadcast_execute = true };
	u8		pdu[] = { FC_WRITE_SINGLE_REGISTER, 0, 0, 0, 0x2A };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u32		len = modbus_rtu_encode(frame, 0, pdu, sizeof(pdu));

	ASSERT_EQ(mogbus_device_add_point(a, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_point(b, &p) == MOGBUS_OK);
	written_log.calls = 0;
	written_log.reentrant = 0;
	ASSERT_EQ(mogbus_device_set_written_hook(a, test_written, NULL) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_written_hook(b, test_written, NULL) == MOGBUS_OK);
	rng_seed(&rng, 1);
	rtu_framer_init(&framer);
	rtu_framer_handle_frame(&framer, &fctx, 0, frame, len);
	ASSERT_EQ(written_log.calls == 2 && !framer.pending.active);
	ASSERT_EQ(written_log.values[0][0] == 0x2A && written_log.values[1][0] == 0x2A);
	mogbus_destroy(ctx);
}

TEST(ApiLastErrorKeepsTheMostRecentFailure)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_point_t	p = holding(0, 1);
	mogbus_device_t* dev;

	ASSERT_EQ(ctx != NULL);
	ASSERT_EQ(mogbus_last_error(ctx)[0] == '\0');
	mogbus_add_device(ctx, 1, "a", &dev);
	ASSERT_EQ(dev != NULL);
	ASSERT_EQ(mogbus_device_add_server(dev, "") == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_last_error(ctx)[0] != '\0');
	/* A call that returns the code reports it itself, and its text replaces the previous one. */
	ASSERT_EQ(mogbus_load_config(ctx, "/nonexistent/mogbus.scm") != MOGBUS_OK);
	ASSERT_EQ(mogbus_last_error(ctx)[0] != '\0');
	/* Success leaves the last error alone. */
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_last_error(ctx)[0] != '\0');
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_ERR_INVALID && strstr(mogbus_last_error(ctx), "already used"));
	ASSERT_EQ(mogbus_probe(NULL, 1, 100) == MOGBUS_ERR_INVALID && mogbus_last_error(NULL)[0] != '\0');
	mogbus_destroy(ctx);
}

TEST(ApiRejectsBadInput)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev;
	mogbus_point_t	p;
	char		name[100];

	ASSERT_EQ(ctx != NULL);
	mogbus_add_device(ctx, 1, NULL, &dev);
	ASSERT_EQ(dev != NULL);

	/* The same unit id is accepted (another server may use it) but one server cannot serve both. */
	ASSERT_EQ(mogbus_add_device(ctx, 1, "again", &(mogbus_device_t*){ NULL }) == MOGBUS_OK);
	{
		mogbus_server_options_t o = mogbus_server_options_defaults();

		o.port = ts_free_port();
		ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK);
		ASSERT_EQ(strstr(mogbus_last_error(ctx), "unit id 1") != NULL);
	}
	memset(name, 'x', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	ASSERT_EQ(mogbus_add_device(ctx, 2, name, &(mogbus_device_t*){ NULL }) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "too long"));
	ASSERT_EQ(mogbus_add_device(ctx, 2, "ok", &(mogbus_device_t*){ NULL }) == MOGBUS_OK);

	p = holding(0, 1);
	p.type = MOGBUS_TYPE_BOOL;					/* bool in a register table */
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK && mogbus_last_error(ctx)[0]);
	p = holding(65535, 1);
	p.type = MOGBUS_TYPE_U32;					/* does not fit */
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	p = holding(5, 1);
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "already used"));
	p = holding(6, 0);
	p.kind = MOGBUS_POINT_RAMP;					/* step 0 */
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	p = holding(6, 0);
	p.value = 1.0 / 0.0;						/* infinity needs float32 */
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_COIL, .address = 1, .type = MOGBUS_TYPE_BOOL,
			      .kind = MOGBUS_POINT_RAMP, .ramp = { .step = 1, .every_ms = 1 } };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);	/* ramping coil */
	p = holding(7, 0);
	p.space = (mogbus_space_t)9;
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);

	for (int unit = 3; unit < MOGBUS_MAX_DEVICES; unit++)
		ASSERT_EQ(mogbus_add_device(ctx, (uint8_t)unit, NULL, &(mogbus_device_t*){ NULL }) == MOGBUS_OK);
	ASSERT_EQ(mogbus_add_device(ctx, 100, NULL, &(mogbus_device_t*){ NULL }) != MOGBUS_OK);
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "too many devices") != NULL);
	mogbus_destroy(ctx);
}

TEST(ApiServesDevicesOverTcpAndRejectsBadServers)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 0x1234);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;
	mogbus_server_t* other;
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	int		fd;

	ASSERT_EQ(ctx != NULL);
	o.port = ts_free_port();
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK);	/* no devices yet */
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "no devices") != NULL);

	mogbus_add_device(ctx, 1, "tcp", &dev);
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	mogbus_server_create(ctx, &o, &server);
	ASSERT_EQ(server != NULL);
	ASSERT_EQ(mogbus_server_pty_name(server) == NULL);
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && mogbus_last_error(ctx)[0]);	/* port in use */

	fd = ts_connect_loopback(o.port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0);
	ASSERT_EQ(ts_mbap_send(fd, 1, 7, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(server, fd));
	ASSERT_EQ(ts_mbap_recv(fd, 1, 7, resp, &len, 1000));
	ASSERT_EQ(len == 4 && resp[2] == 0x12 && resp[3] == 0x34);

	/* A host change shows up on the wire. */
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 0, 0x4321) == MOGBUS_OK);
	ASSERT_EQ(ts_mbap_send(fd, 1, 8, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(server, fd));
	ASSERT_EQ(ts_mbap_recv(fd, 1, 8, resp, &len, 1000));
	ASSERT_EQ(resp[2] == 0x43 && resp[3] == 0x21);
	close(fd);

	mogbus_server_stop(server);
	ASSERT_EQ(mogbus_server_step(server, 0) == MOGBUS_STEP_DONE);
	mogbus_server_close(server);
	mogbus_server_close(NULL);

	/* The slot and the port are free again. */
	mogbus_server_create(ctx, &o, &other);
	ASSERT_EQ(other != NULL);
	o.mode = MOGBUS_MODE_RTU_DEVICE;
	o.path = NULL;
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "path"));
	o.mode = MOGBUS_MODE_ASCII_FD;
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "fd"));
	o = mogbus_server_options_defaults();
	o.tls_enabled = true;	/* unreadable files, or a build without TLS */
	o.tls_certificate = "/nonexistent/cert.pem";
	o.tls_private_key = "/nonexistent/key.pem";
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && mogbus_last_error(ctx)[0]);
	o = mogbus_server_options_defaults();
	o.bind_address = "01234567890123456789012345678901234567890123456789012345678901234567890123456789"
			 "01234567890123456789012345678901234567890123456789";
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "too long"));
	mogbus_destroy(ctx);	/* closes `other` */
}

/* Runs mogbus_probe() == MOGBUS_OK in a child (it blocks) while this process steps the server. */
static bool
probe_child(mogbus_server_t* server, const mogbus_server_options_t* o, uint8_t unit)
{
	u64	deadline = os_now_ms() + 5000;
	int	status   = 0;
	pid_t	pid      = fork();

	if (pid == 0)
		_exit(mogbus_probe(o, unit, 2000) == MOGBUS_OK ? 0 : 1);
	if (pid < 0)
		return false;
	while (waitpid(pid, &status, WNOHANG) == 0)
	{
		if (os_now_ms() > deadline)
		{
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return false;
		}
		if (server)
			(void)mogbus_server_step(server, 1);
		else
			usleep(1000);
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

TEST(ApiProbeAnswersOnEveryNetworkMode)
{
	static const mogbus_mode_t modes[] = {
		MOGBUS_MODE_TCP, MOGBUS_MODE_UDP, MOGBUS_MODE_RTU_TCP, MOGBUS_MODE_RTU_UDP,
		MOGBUS_MODE_ASCII_TCP, MOGBUS_MODE_ASCII_UDP,
	};

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
	{
		mogbus_t*	ctx = NULL;
		mogbus_create(&ctx);
		mogbus_server_options_t o = mogbus_server_options_defaults();
		mogbus_server_t* server;

		ASSERT_EQ(ctx != NULL && mogbus_add_device(ctx, 9, "probe", &(mogbus_device_t*){ NULL }) == MOGBUS_OK);
		o.mode = modes[i];
		o.port = ts_free_port();
		mogbus_server_create(ctx, &o, &server);
		ASSERT_EQ(server != NULL);
		ASSERT_EQ(probe_child(server, &o, 9));
		o.bind_address = "0.0.0.0";	/* a wildcard bind is probed through loopback */
		ASSERT_EQ(probe_child(server, &o, 9));
		mogbus_destroy(ctx);
	}
}

TEST(ApiProbeFailsWithoutAServer)
{
	mogbus_server_options_t o = mogbus_server_options_defaults();

	o.port = ts_free_port();
	ASSERT_EQ(mogbus_probe(&o, 1, 500) != MOGBUS_OK && strstr(mogbus_last_error(NULL), "connect") != NULL);
	o.mode = MOGBUS_MODE_UDP;
	ASSERT_EQ(mogbus_probe(&o, 1, 500) != MOGBUS_OK && mogbus_last_error(NULL)[0]);
	o.mode = MOGBUS_MODE_RTU_PTY;
	ASSERT_EQ(mogbus_probe(&o, 1, 500) != MOGBUS_OK && strstr(mogbus_last_error(NULL), "network mode") != NULL);
	o.mode = MOGBUS_MODE_TCP;
	ASSERT_EQ(mogbus_probe(&o, 0, 500) != MOGBUS_OK && strstr(mogbus_last_error(NULL), "broadcast") != NULL);
	ASSERT_EQ(mogbus_probe(NULL, 1, 500) != MOGBUS_OK);
}

TEST(ApiProbeFailsWhenTheUnitIsUnknownOrSilent)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;

	ASSERT_EQ(ctx != NULL && mogbus_add_device(ctx, 9, "probe", &(mogbus_device_t*){ NULL }) == MOGBUS_OK);
	o.mode = MOGBUS_MODE_RTU_TCP;	/* an unknown RTU unit gets no reply */
	o.port = ts_free_port();
	mogbus_server_create(ctx, &o, &server);
	ASSERT_EQ(server != NULL);
	ASSERT_EQ(!probe_child(server, &o, 10));
	ASSERT_EQ(probe_child(server, &o, 9));
	mogbus_destroy(ctx);
}

/* Steps the server until `fd` has a full HTTP response (the endpoint closes after it). */
static bool
http_get(mogbus_server_t* server, u16 port, const char* path, char* out, u32 cap)
{
	char	req[128];
	int	fd = ts_connect_loopback(port, AF_INET, 1000);
	u32	len = 0;
	u64	deadline = os_now_ms() + 2000;

	if (fd < 0)
		return false;
	snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", path);
	if (send(fd, req, strlen(req), MSG_NOSIGNAL) != (ssize_t)strlen(req))
	{
		close(fd);
		return false;
	}
	while (os_now_ms() < deadline && len + 1 < cap)
	{
		struct pollfd	pfd = { .fd = fd, .events = POLLIN };
		ssize_t		got;

		(void)mogbus_server_step(server, 5);
		if (poll(&pfd, 1, 0) <= 0)
			continue;
		got = recv(fd, out + len, cap - 1 - len, 0);
		if (got <= 0)
			break;
		len += (u32)got;
	}
	out[len] = '\0';
	close(fd);
	return len > 0;
}

TEST(ApiServesHealthAndMetricsOverHttp)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 0x1234);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_options_t clash;
	mogbus_server_t* server;
	static char	text[40000];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	int		fd;

	ASSERT_EQ(ctx != NULL);
	mogbus_add_device(ctx, 1, "http", &dev);
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(o.http_bind_address == NULL && o.http_port == 9102);
	ASSERT_EQ(!o.http_ui && o.http_allowed_hosts == NULL);
	o.port              = ts_free_port();
	o.http_bind_address = "127.0.0.1";
	o.http_port         = ts_free_port();
	mogbus_server_create(ctx, &o, &server);
	ASSERT_EQ(server != NULL);

	fd = ts_connect_loopback(o.port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0 && ts_mbap_send(fd, 1, 9, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(server, fd) && ts_mbap_recv(fd, 1, 9, resp, &len, 1000));
	close(fd);

	ASSERT_EQ(http_get(server, o.http_port, "/healthz", text, sizeof(text)));
	ASSERT_EQ(strncmp(text, "HTTP/1.1 200 OK", 15) == 0);
	ASSERT_EQ(http_get(server, o.http_port, "/readyz", text, sizeof(text)));
	ASSERT_EQ(strncmp(text, "HTTP/1.1 200 OK", 15) == 0);
	ASSERT_EQ(http_get(server, o.http_port, "/metrics", text, sizeof(text)));
	ASSERT_EQ(strstr(text, "\nmogbus_requests_total{fc=\"3\"} 1\n") != NULL);
	ASSERT_EQ(strstr(text, "\nmogbus_device_responses_total{unit=\"1\",result=\"ok\"} 1\n") != NULL);
	ASSERT_EQ(strstr(text, "\nmogbus_connections_total 1\n") != NULL);

	ASSERT_EQ(mogbus_server_metrics(server, text, sizeof(text)) == strlen(text));
	ASSERT_EQ(strstr(text, "\nmogbus_ready 1\n") != NULL);
	ASSERT_EQ(mogbus_server_metrics(server, text, 16) == 0);

	/* The endpoint port is taken now: a second server fails like a busy listener. */
	clash                   = o;
	clash.port              = ts_free_port();
	ASSERT_EQ(mogbus_server_create(ctx, &clash, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_LISTEN);
	clash.http_bind_address = "localhost";
	ASSERT_EQ(mogbus_server_create(ctx, &clash, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_INVALID && strstr(mogbus_last_error(ctx), "HTTP bind address") != NULL);
	clash.http_bind_address = "127.0.0.1";
	clash.http_allowed_hosts = "a b";
	ASSERT_EQ(mogbus_server_check(ctx, &clash) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "allowed hosts") != NULL);
	clash.http_allowed_hosts = "sim.lab,mogbus";
	ASSERT_EQ(mogbus_server_check(ctx, &clash) == MOGBUS_OK);

	mogbus_server_stop(server);
	ASSERT_EQ(mogbus_server_step(server, 0) == MOGBUS_STEP_DONE);
	mogbus_server_close(server);
	mogbus_destroy(ctx);
}

TEST(ApiServesDevicesOverDatagramModes)
{
	static const mogbus_mode_t modes[] = { MOGBUS_MODE_UDP, MOGBUS_MODE_ASCII_UDP, MOGBUS_MODE_RTU_UDP };

	for (u32 c = 0; c < sizeof(modes) / sizeof(modes[0]); c++)
	{
		mogbus_t*	ctx = NULL;
		mogbus_create(&ctx);
		mogbus_device_t* dev;
		mogbus_point_t	p = holding(0, 0x1234);
		mogbus_server_options_t o = mogbus_server_options_defaults();
		mogbus_server_t* server;
		u8		raw[MODBUS_ASCII_MAX_FRAME];
		u8		decoded[MODBUS_MAX_PDU + 2];
		u8		addr = 1;
		const u8*	pdu = NULL;
		u32		pdu_len = 0;
		s32		got;
		int		fd;

		ASSERT_EQ(ctx != NULL);
		mogbus_add_device(ctx, 1, "datagram", &dev);
		ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p) == MOGBUS_OK);
		o.mode = modes[c];
		o.port = ts_free_port();
		mogbus_server_create(ctx, &o, &server);
		ASSERT_EQ(server != NULL);

		fd = ts_udp_loopback(o.port, AF_INET, 1000);
		ASSERT_EQ(fd >= 0);
		if (modes[c] == MOGBUS_MODE_UDP)
		{
			ASSERT_EQ(ts_mbap_send(fd, 1, 7, READ_HOLDING, sizeof(READ_HOLDING)));
			ASSERT_EQ(pump(server, fd));
			ASSERT_EQ(ts_udp_mbap_recv(fd, 1, 7, decoded, &pdu_len, 1000));
			pdu = decoded;
		}
		else
		{
			got = (s32)(modes[c] == MOGBUS_MODE_ASCII_UDP
				    ? modbus_ascii_encode(raw, 1, READ_HOLDING, sizeof(READ_HOLDING))
				    : modbus_rtu_encode(raw, 1, READ_HOLDING, sizeof(READ_HOLDING)));
			ASSERT_EQ(got > 0 && send(fd, raw, (size_t)got, 0) == got);
			ASSERT_EQ(pump(server, fd));
			got = ts_udp_recv(fd, raw, sizeof(raw), 1000);
			ASSERT_EQ(got > 0);
			if (modes[c] == MOGBUS_MODE_ASCII_UDP)
				ASSERT_EQ(modbus_ascii_decode(raw, (u32)got, decoded, &addr, &pdu, &pdu_len));
			else
				ASSERT_EQ(modbus_rtu_decode(raw, (u32)got, &addr, &pdu, &pdu_len));
		}
		ASSERT_EQ(addr == 1 && pdu_len == 4 && pdu[2] == 0x12 && pdu[3] == 0x34);
		close(fd);

		mogbus_server_stop(server);
		ASSERT_EQ(mogbus_server_step(server, 0) == MOGBUS_STEP_DONE);
		mogbus_server_close(server);

		o.mode = (mogbus_mode_t)(MOGBUS_MODE_RTU_UDP + 1);
		ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && mogbus_last_error(ctx)[0]);
		o.mode = (mogbus_mode_t)99;
		ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && mogbus_last_error(ctx)[0]);
		mogbus_destroy(ctx);
	}
}

TEST(ApiAnyUnitAllowsOnlyOneDeviceAndShowsInInfo)
{
	mogbus_t*		ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t*	a = NULL;
	mogbus_add_device(ctx, 1, "a", &a);
	mogbus_device_t*	b = NULL;
	mogbus_add_device(ctx, 2, "b", &b);
	mogbus_device_info_t	info;

	ASSERT_EQ(a != NULL && b != NULL);
	ASSERT_EQ(mogbus_device_info(a, &info) && !info.any_unit);
	ASSERT_EQ(mogbus_device_set_any_unit(a, true) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_info(a, &info) && info.any_unit);
	ASSERT_EQ(mogbus_device_set_any_unit(b, true) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_any_unit(a, true) == MOGBUS_OK);	/* idempotent */
	ASSERT_EQ(mogbus_device_set_any_unit(b, false) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_any_unit(a, false) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_any_unit(b, true) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_info(b, &info) && info.any_unit);
	ASSERT_EQ(mogbus_device_set_any_unit(NULL, true) != MOGBUS_OK);
	mogbus_destroy(ctx);
}

#if WITH_CONFIG

TEST(ApiLoadsConfigAndServesFromIt)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	char		text[256];
	int		cfd = mkstemp(path);
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev;
	mogbus_server_t* server;
	u16		port = ts_free_port();
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	double		v = 0;
	int		fd;

	ASSERT_EQ(cfd >= 0 && ctx != NULL);
	snprintf(text, sizeof(text),
		 "(server (mode tcp) (port %u))\n"
		 "(device (name \"cfg\") (unit-id 4) (holding 0 (static 77)))\n", port);
	ASSERT_EQ(write(cfd, text, strlen(text)) == (ssize_t)strlen(text));
	close(cfd);

	ASSERT_EQ(mogbus_server_create_from_config(ctx, &(mogbus_server_t*){ NULL }) != MOGBUS_OK);	/* nothing loaded */
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "no config") != NULL);
	ASSERT_EQ(mogbus_load_config(ctx, "/nonexistent/mogbus.scm") != MOGBUS_OK && mogbus_last_error(ctx)[0]);
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	dev = mogbus_find_device(ctx, 4);
	ASSERT_EQ(dev != NULL && mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) == MOGBUS_OK && v == 77);

	/* Loading again would duplicate unit 4: rejected, context unchanged. */
	ASSERT_EQ(mogbus_load_config(ctx, path) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "unit id 4"));
	ASSERT_EQ(mogbus_find_device(ctx, 4) == dev);

	/* Devices built in code are served together with the loaded ones. */
	{
		mogbus_device_t* extra = NULL;
		mogbus_add_device(ctx, 5, "extra", &extra);
		mogbus_point_t p = holding(0, 5);

		ASSERT_EQ(extra != NULL && mogbus_device_add_point(extra, &p) == MOGBUS_OK);
	}
	mogbus_server_create_from_config(ctx, &server);
	ASSERT_EQ(server != NULL);
	fd = ts_connect_loopback(port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0);
	ASSERT_EQ(ts_mbap_send(fd, 5, 1, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(server, fd));
	ASSERT_EQ(ts_mbap_recv(fd, 5, 1, resp, &len, 1000));
	ASSERT_EQ(len == 4 && resp[3] == 5);
	ASSERT_EQ(ts_mbap_send(fd, 4, 2, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(server, fd));
	ASSERT_EQ(ts_mbap_recv(fd, 4, 2, resp, &len, 1000));
	ASSERT_EQ(len == 4 && resp[3] == 77);
	close(fd);
	mogbus_destroy(ctx);
	unlink(path);
}
#else
TEST(ApiConfigFunctionsFailWithoutConfigSupport)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);

	ASSERT_EQ(ctx != NULL);
	ASSERT_EQ(mogbus_load_config(ctx, "/nonexistent/mogbus.scm") != MOGBUS_OK);
	ASSERT_EQ(strcmp(mogbus_last_error(ctx), "built without config support") == 0);
	ASSERT_EQ(mogbus_server_create_from_config(ctx, &(mogbus_server_t*){ NULL }) != MOGBUS_OK);
	ASSERT_EQ(strcmp(mogbus_last_error(ctx), "built without config support") == 0);
	mogbus_destroy(ctx);
}
#endif

TEST(ApiServesAnExistingFdInRtuAndAsciiModes)
{
	static const mogbus_mode_t modes[] = { MOGBUS_MODE_RTU_FD, MOGBUS_MODE_ASCII_FD };

	for (u32 i = 0; i < 2; i++)
	{
		bool		ascii = modes[i] == MOGBUS_MODE_ASCII_FD;
		mogbus_t*	ctx = NULL;
		mogbus_create(&ctx);
		mogbus_device_t* dev = NULL;
		mogbus_add_device(ctx, 3, "fd", &dev);
		mogbus_point_t	p = holding(0, 0xABCD);
		mogbus_server_options_t o = mogbus_server_options_defaults();
		mogbus_server_t* server;
		int		pair[2];
		u8		resp[MODBUS_MAX_PDU];
		u8		addr = 0;
		u32		len = 0;
		bool		sent;

		ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p) == MOGBUS_OK);
		ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
		o.mode = modes[i];
		o.fd = pair[1];
		o.frame_gap_ms = 2;
		mogbus_server_create(ctx, &o, &server);
		ASSERT_EQ(server != NULL);
		sent = ascii ? ts_ascii_send(pair[0], 3, READ_HOLDING, sizeof(READ_HOLDING), true) :
			       ts_rtu_send(pair[0], 3, READ_HOLDING, sizeof(READ_HOLDING), true);
		ASSERT_EQ(sent);
		ASSERT_EQ(pump(server, pair[0]));
		if (ascii)
			ASSERT_EQ(ts_ascii_recv_response(pair[0], &addr, resp, &len, 500, true));
		else
			ASSERT_EQ(ts_rtu_recv(pair[0], &addr, resp, &len, 500, true));
		ASSERT_EQ(addr == 3 && len == 4 && resp[2] == 0xAB && resp[3] == 0xCD);

		mogbus_server_stop(server);
		ASSERT_EQ(mogbus_server_run(server) == 0);	/* returns at once */
		mogbus_server_close(server);
		ASSERT_EQ(write(pair[1], "x", 1) == 1);		/* the fd stayed ours */
		close(pair[0]);
		close(pair[1]);
		mogbus_destroy(ctx);
	}
}

typedef struct sink_t sink_t;
struct sink_t
{
	unsigned		count;
	mogbus_log_level_t	level;
	char			last[256];
};

static void
sink_fn(void* user, mogbus_log_level_t level, const char* message)
{
	sink_t* s = user;

	s->count++;
	s->level = level;
	snprintf(s->last, sizeof(s->last), "%s", message);
}

TEST(ApiLogSinkReceivesServerLogs)
{
	sink_t		sink = { 0 };
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev = NULL;
	mogbus_add_device(ctx, 1, NULL, &dev);
	mogbus_point_t	p = holding(0, 1);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;

	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	o.port = ts_free_port();
	mogbus_set_log_sink(sink_fn, &sink);
	mogbus_server_create(ctx, &o, &server);
	ASSERT_EQ(server != NULL);
	ASSERT_EQ(sink.count >= 1 && sink.level == MOGBUS_LOG_INFO);
	ASSERT_EQ(strstr(sink.last, "listening") != NULL);

	/* Below the level nothing is delivered; closing logs at INFO. */
	mogbus_set_log_level(MOGBUS_LOG_WARN);
	{
		unsigned before = sink.count;

		mogbus_server_close(server);
		ASSERT_EQ(sink.count == before);
	}
	mogbus_set_log_level(MOGBUS_LOG_INFO);
	mogbus_set_log_sink(NULL, NULL);
	mogbus_destroy(ctx);
}

TEST(ApiRtuTimingMatchesTheServer)
{
	uint32_t t35 = 99;
	uint32_t gap = 99;

	mogbus_rtu_timing(9600, 0, &t35, &gap);
	ASSERT_EQ(t35 == 4011 && gap == 5);
	mogbus_rtu_timing(19200, 0, &t35, &gap);
	ASSERT_EQ(t35 == 2006 && gap == 3);
	mogbus_rtu_timing(115200, 0, &t35, &gap);
	ASSERT_EQ(t35 == 1750 && gap == 2);
	mogbus_rtu_timing(9600, 12, &t35, &gap);	/* an explicit gap wins over the baud */
	ASSERT_EQ(t35 == 4011 && gap == 12);
	mogbus_rtu_timing(0, 0, &t35, &gap);		/* unknown baud: the default gap */
	ASSERT_EQ(t35 == 0 && gap == 20);
	mogbus_rtu_timing(9600, 0, NULL, &gap);
	mogbus_rtu_timing(9600, 0, &t35, NULL);
	ASSERT_EQ(gap == 5 && t35 == 4011);
}

TEST(ApiFailuresSetOutToNullAndKeepText)
{
	mogbus_t*		ctx    = NULL;
	mogbus_device_t*	dev    = (mogbus_device_t*)1;
	mogbus_server_t*	server = (mogbus_server_t*)1;
	mogbus_server_options_t	o      = mogbus_server_options_defaults();
	uint16_t		words[1];

	ASSERT_EQ(mogbus_create(&ctx) == MOGBUS_OK && ctx != NULL);
	ASSERT_EQ(mogbus_add_device(ctx, 1, "this name is much too long for a device, it has more than 63 bytes", &dev) ==
		  MOGBUS_ERR_INVALID);
	ASSERT_EQ(dev == NULL && strstr(mogbus_last_error(ctx), "too long") != NULL);
	ASSERT_EQ(mogbus_server_create(ctx, &o, &server) == MOGBUS_ERR_INVALID);	/* no devices */
	ASSERT_EQ(server == NULL && strstr(mogbus_last_error(ctx), "no devices") != NULL);
	ASSERT_EQ(mogbus_server_create_from_config(ctx, &server) != MOGBUS_OK && server == NULL);
	ASSERT_EQ(mogbus_add_device(ctx, 1, "ok", &dev) == MOGBUS_OK && dev != NULL);
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 1, words) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "undefined") != NULL);
	mogbus_destroy(ctx);
}

TEST(ApiServerCheckValidatesWithoutOpeningAnything)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 1);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;

	ASSERT_EQ(ctx != NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "no devices"));
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_INVALID);
	mogbus_add_device(ctx, 1, "chk", &dev);
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p) == MOGBUS_OK);

	/* A port that is already in use passes the check, but not the create. */
	o.port = ts_free_port();
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
	mogbus_server_create(ctx, &o, &server);
	ASSERT_EQ(server != NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_LISTEN);

	/* A missing serial device passes the check (nothing is opened) and fails on create. */
	o = mogbus_server_options_defaults();
	o.mode = MOGBUS_MODE_RTU_DEVICE;
	o.path = "/nonexistent/ttyMOGBUS";
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_SERIAL && strstr(mogbus_last_error(ctx), "serial") != NULL);
	o.path = NULL;
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "path"));
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_INVALID);
	o = mogbus_server_options_defaults();
	o.stop_bits = 3;
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "serial line"));
	o = mogbus_server_options_defaults();
	o.bind_address = "01234567890123456789012345678901234567890123456789012345678901234567890123456789"
			 "01234567890123456789012345678901234567890123456789";
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "too long"));
	o = mogbus_server_options_defaults();
	o.mode = (mogbus_mode_t)99;
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK && mogbus_last_error(ctx)[0]);

	/* TLS files are loaded for the check (unreadable here, or a build without TLS). */
	o = mogbus_server_options_defaults();
	o.tls_enabled = true;
	o.tls_certificate = "/nonexistent/cert.pem";
	o.tls_private_key = "/nonexistent/key.pem";
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK && mogbus_last_error(ctx)[0]);
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK);
	ASSERT_EQ(mogbus_server_create(ctx, &o, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_LISTEN);
#if WITH_TLS
	o.tls_certificate = "tests/tls/server.pem";
	o.tls_private_key = "tests/tls/server.key";
	o.tls_ca_certificate = "tests/tls/ca.pem";
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
	o.tls_ca_certificate = NULL;	/* client certificates are required by default */
	ASSERT_EQ(mogbus_server_check(ctx, &o) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "CA"));
	o.tls_require_client_certificate = false;
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
#endif
	mogbus_destroy(ctx);
}

#if WITH_CONFIG
static bool
write_config(char* path, const char* text)
{
	int fd = mkstemp(path);
	bool ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);

	if (fd >= 0)
		close(fd);
	return ok;
}

TEST(ApiConfigServerOptionsRoundTrip)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_server_options_t o;

	ASSERT_EQ(ctx != NULL && mogbus_config_server_options(ctx, &o) != MOGBUS_OK);	/* nothing loaded */
	ASSERT_EQ(write_config(path,
		"(server (mode rtu-device) (path \"/dev/ttyUSB9\") (baud 19200)\n"
		"        (parity even) (stop-bits 2) (strict-t15 on) (broadcast-execute on)\n"
		"        (seed 42))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	ASSERT_EQ(mogbus_config_server_options(ctx, &o) == MOGBUS_OK);
	ASSERT_EQ(o.mode == MOGBUS_MODE_RTU_DEVICE && strcmp(o.path, "/dev/ttyUSB9") == 0);
	ASSERT_EQ(o.baud == 19200 && o.frame_gap_ms == 0 && o.seed == 42);
	ASSERT_EQ(o.data_bits == 8 && o.parity == MOGBUS_PARITY_EVEN && o.stop_bits == 2);
	ASSERT_EQ(o.strict_t15 && o.broadcast_execute && !o.tls_enabled && o.fd == -1);
	mogbus_destroy(ctx);
	unlink(path);

	mogbus_create(&ctx);
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode ascii-device) (path \"/dev/ttyUSB8\") (data-bits 7) (parity even))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	ASSERT_EQ(mogbus_config_server_options(ctx, &o) == MOGBUS_OK);
	ASSERT_EQ(o.mode == MOGBUS_MODE_ASCII_DEVICE && o.data_bits == 7 && o.stop_bits == 1);
	mogbus_destroy(ctx);
	unlink(path);

	mogbus_create(&ctx);
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode ascii-tcp) (port 4321) (bind \"0.0.0.0\") (idle-timeout-ms 1500))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	ASSERT_EQ(mogbus_config_server_options(ctx, &o) == MOGBUS_OK);
	ASSERT_EQ(o.mode == MOGBUS_MODE_ASCII_TCP && o.port == 4321 && o.idle_timeout_ms == 1500);
	ASSERT_EQ(strcmp(o.bind_address, "0.0.0.0") == 0 && o.path == NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
	mogbus_destroy(ctx);
	unlink(path);

#if WITH_TLS
	mogbus_create(&ctx);
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode tcp) (port 4802) (tls (certificate \"tests/tls/server.pem\")\n"
		"  (private-key \"tests/tls/server.key\") (ca-certificate \"tests/tls/ca.pem\")\n"
		"  (handshake-timeout-ms 777) (min-version tls1.3)))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	ASSERT_EQ(mogbus_config_server_options(ctx, &o) == MOGBUS_OK);
	ASSERT_EQ(o.mode == MOGBUS_MODE_TCP && o.port == 4802 && o.tls_enabled);
	ASSERT_EQ(strcmp(o.tls_certificate, "tests/tls/server.pem") == 0);
	ASSERT_EQ(strcmp(o.tls_private_key, "tests/tls/server.key") == 0);
	ASSERT_EQ(strcmp(o.tls_ca_certificate, "tests/tls/ca.pem") == 0);
	ASSERT_EQ(o.tls_handshake_timeout_ms == 777 && o.tls_require_client_certificate);
	ASSERT_EQ(o.tls_min_version == MOGBUS_TLS_1_3 && o.tls_ciphers_1_2 == NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
	mogbus_destroy(ctx);
	unlink(path);

	/* Without a CA and without required client certificates TLS still works. */
	mogbus_create(&ctx);
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode tcp) (tls (certificate \"tests/tls/server.pem\")\n"
		"  (private-key \"tests/tls/server.key\") (require-client-certificate off)))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	ASSERT_EQ(mogbus_config_server_options(ctx, &o) == MOGBUS_OK && o.tls_ca_certificate == NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o) == MOGBUS_OK);
	mogbus_destroy(ctx);
	unlink(path);
#endif
}

TEST(ApiEnumeratesDevicesAndDescribesThem)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_info_t info;
	mogbus_point_t	p = holding(0, 1);

	ASSERT_EQ(mogbus_device_count(ctx) == 0 && mogbus_device_at(ctx, 0) == NULL);
	ASSERT_EQ(write_config(path,
		"(device (name \"meter\") (unit-id 3)\n"
		"  (coil 0 (static 1)) (coil 1 (static 0)) (discrete 5 (static 1))\n"
		"  (input 0 (static 1)) (holding 0 (static 2)) (holding 1 (static 3)) (holding 2 (static 4))\n"
		"  (fault (kind delay) (percent 25) (delay-ms 40))\n"
		"  (fault (kind exception) (code 6) (percent 10))\n"
		"  (fault (kind drop) (schedule (step (after-ms 100) (count 2))))\n"
		"  (fault (kind sensor-disconnect) (percent 100)))\n"
		"(device (unit-id 9) (holding 0) (input 0 (sine (offset 5) (amplitude 1) (period-ms 1000))))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	{
		mogbus_device_t* extra = NULL;
		mogbus_add_device(ctx, 11, "code", &extra);

		ASSERT_EQ(extra != NULL && mogbus_device_add_point(extra, &p) == MOGBUS_OK);
	}
	ASSERT_EQ(mogbus_device_count(ctx) == 3);
	ASSERT_EQ(mogbus_device_at(ctx, 3) == NULL);
	ASSERT_EQ(mogbus_device_at(ctx, 0) == mogbus_find_device(ctx, 3));
	ASSERT_EQ(mogbus_device_at(ctx, 2) == mogbus_find_device(ctx, 11));

	ASSERT_EQ(mogbus_device_info(mogbus_device_at(ctx, 0), &info));
	ASSERT_EQ(strcmp(info.name, "meter") == 0 && info.unit_id == 3);
	ASSERT_EQ(info.point_count[MOGBUS_COIL] == 2 && info.point_count[MOGBUS_DISCRETE] == 1);
	ASSERT_EQ(info.point_count[MOGBUS_INPUT] == 1 && info.point_count[MOGBUS_HOLDING] == 3);
	ASSERT_EQ(info.faults[MOGBUS_FAULT_DELAY].present && !info.faults[MOGBUS_FAULT_DELAY].scheduled);
	ASSERT_EQ(info.faults[MOGBUS_FAULT_DELAY].percent == 25 && info.faults[MOGBUS_FAULT_DELAY].delay_ms == 40);
	ASSERT_EQ(info.faults[MOGBUS_FAULT_EXCEPTION].present && info.faults[MOGBUS_FAULT_EXCEPTION].percent == 10);
	ASSERT_EQ(info.faults[MOGBUS_FAULT_EXCEPTION].exception_code == 6);
	ASSERT_EQ(info.faults[MOGBUS_FAULT_DROP].present && info.faults[MOGBUS_FAULT_DROP].scheduled);
	ASSERT_EQ(info.faults[MOGBUS_FAULT_SENSOR_DISCONNECT].present);
	ASSERT_EQ(!info.faults[MOGBUS_FAULT_CORRUPT].present && !info.faults[MOGBUS_FAULT_DISCONNECT].present);
	ASSERT_EQ(!info.faults[MOGBUS_FAULT_RESTART].present);

	ASSERT_EQ(mogbus_device_info(mogbus_device_at(ctx, 2), &info));
	ASSERT_EQ(strcmp(info.name, "code") == 0 && info.point_count[MOGBUS_HOLDING] == 1);
	ASSERT_EQ(!info.faults[MOGBUS_FAULT_DELAY].present && !mogbus_device_info(NULL, &info));
	{
		mogbus_point_t pt;
		uint32_t       n = 0;

		/* 7 points in definition order (two coils, discrete, input, three holding), then the end. */
		while (mogbus_device_point_at(mogbus_device_at(ctx, 0), n, &pt))
			n++;
		ASSERT_EQ(n == 7);
		ASSERT_EQ(mogbus_device_point_at(mogbus_device_at(ctx, 0), 0, &pt));
		ASSERT_EQ(pt.space == MOGBUS_COIL && pt.address == 0 && pt.type == MOGBUS_TYPE_BOOL);
		ASSERT_EQ(mogbus_device_point_at(mogbus_device_at(ctx, 0), 6, &pt));
		ASSERT_EQ(pt.space == MOGBUS_HOLDING && pt.address == 2 && pt.type == MOGBUS_TYPE_U16 &&
			  pt.kind == MOGBUS_POINT_STATIC);
		ASSERT_EQ(mogbus_device_point_at(mogbus_device_at(ctx, 1), 1, &pt));
		ASSERT_EQ(pt.space == MOGBUS_INPUT && pt.kind == MOGBUS_POINT_MODEL);
		ASSERT_EQ(!mogbus_device_point_at(NULL, 0, &pt) &&
			  !mogbus_device_point_at(mogbus_device_at(ctx, 0), 0, NULL));
	}
	mogbus_destroy(ctx);
	unlink(path);
}

TEST(ApiLoadConfigRejectsSecondAnyUnitDevice)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* a = NULL;
	mogbus_add_device(ctx, 1, "a", &a);

	ASSERT_EQ(write_config(path, "(device (name \"b\") (unit-id 2) (any-unit on) (holding 0))\n"));
	ASSERT_EQ(a != NULL && mogbus_device_set_any_unit(a, true) == MOGBUS_OK);
	ASSERT_EQ(mogbus_load_config(ctx, path) != MOGBUS_OK);
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "both set any-unit") != NULL && mogbus_device_count(ctx) == 1);
	ASSERT_EQ(mogbus_device_set_any_unit(a, false) == MOGBUS_OK);
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_any_unit(a, true) != MOGBUS_OK);
	mogbus_destroy(ctx);
	unlink(path);
}
#else
TEST(ApiConfigOptionsAreUnavailableWithoutConfigSupport)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_server_options_t o;

	ASSERT_EQ(ctx != NULL && mogbus_config_server_options(ctx, &o) != MOGBUS_OK);
	mogbus_destroy(ctx);
}
#endif

TEST(ApiSixtyFourBitByteOrderAndStringPoints)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev;
	mogbus_point_t	p;
	uint16_t	w[4] = { 0 };
	double		v = 0;
	char		text[16];

	ASSERT_EQ(ctx != NULL);
	mogbus_add_device(ctx, 1, NULL, &dev);
	ASSERT_EQ(dev != NULL);

	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_U64,
			      .value = 4294967296.0, .byte_order = MOGBUS_BYTES_LITTLE };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 4, w) == MOGBUS_OK);
	ASSERT_EQ(w[0] == 0 && w[1] == 0x0100 && w[2] == 0 && w[3] == 0);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) == MOGBUS_OK && v == 4294967296.0);
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 4, .type = MOGBUS_TYPE_F64,
			      .value = -2.5, .word_order = MOGBUS_WORDS_LOW_FIRST };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 4, 4, w) == MOGBUS_OK);
	ASSERT_EQ(w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0xC004);
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 8, .type = MOGBUS_TYPE_S64, .value = -1 };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 8, &v) == MOGBUS_OK && v == -1);

	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 20, .type = MOGBUS_TYPE_STRING,
			      .length = 4, .text = "SN-1234" };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, sizeof(text)) == MOGBUS_OK);
	ASSERT_EQ(strcmp(text, "SN-1234") == 0);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 20, &v) != MOGBUS_OK);	/* a string has no number */
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 20, 1) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, 4) != MOGBUS_OK);	/* too small */
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 0, text, sizeof(text)) != MOGBUS_OK);	/* not a string */
	ASSERT_EQ(mogbus_device_set_string(dev, MOGBUS_HOLDING, 20, "NEW") == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, sizeof(text)) == MOGBUS_OK);
	ASSERT_EQ(strcmp(text, "NEW") == 0);
	ASSERT_EQ(mogbus_device_set_string(dev, MOGBUS_HOLDING, 20, "123456789") != MOGBUS_OK);	/* > 2*length */
	ASSERT_EQ(mogbus_device_set_string(dev, MOGBUS_HOLDING, 21, "A") != MOGBUS_OK);
	/* A client write still works and shows up in get_string. */
	w[0] = 0x4F4B;
	w[1] = w[2] = w[3] = 0;
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 20, 4, w) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, sizeof(text)) == MOGBUS_OK);
	ASSERT_EQ(strcmp(text, "OK") == 0);

	/* strings are limited to 1..123 registers and need a register table */
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 100, .type = MOGBUS_TYPE_STRING };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "between 1 and 123"));
	p.length = 124;
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 100, .type = MOGBUS_TYPE_STRING,
			      .length = 1, .text = "TOO LONG" };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_COIL, .address = 0, .type = MOGBUS_TYPE_STRING, .length = 1 };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 100, .type = MOGBUS_TYPE_U16,
			      .byte_order = (mogbus_byte_order_t)7 };
	ASSERT_EQ(mogbus_device_add_point(dev, &p) != MOGBUS_OK);
	mogbus_destroy(ctx);
}

/* ---- host API for file records, FIFO queues and CANopen objects, and their hook ---- */

static struct target_log_t
{
	int		calls;
	mogbus_access_t	seen[8];
	uint16_t	values[8][4];
	uint8_t		data[8][4];
	int		veto_call;	/* 1-based call that answers `answer`; 0 = never */
	uint8_t		answer;
	bool		refresh;	/* reads: replace the target's content first */
} target_log;

static uint8_t
target_hook(void* user, mogbus_device_t* dev, const mogbus_access_t* a)
{
	int	n = target_log.calls++;

	(void)user;
	if (n < 8)
	{
		target_log.seen[n] = *a;
		if (a->target == MOGBUS_TARGET_FILE_RECORD &&
		    a->values)
			memcpy(target_log.values[n], a->values, a->count * sizeof(uint16_t));
		if (a->target == MOGBUS_TARGET_CANOPEN &&
		    a->data)
			memcpy(target_log.data[n], a->data, a->data_len);
	}
	if (target_log.veto_call == n + 1)
		return target_log.answer;
	if (target_log.refresh &&
	    a->kind == MOGBUS_ACCESS_READ)
	{
		uint16_t	fifo[2]   = { 7, 8 };
		uint16_t	record[1] = { 99 };
		uint8_t		bytes[2]  = { 0xAB, 0xCD };

		if (a->target == MOGBUS_TARGET_FIFO)
			(void)mogbus_device_set_fifo(dev, (uint16_t)a->address, 2, fifo);
		else if (a->target == MOGBUS_TARGET_FILE_RECORD)
			(void)mogbus_device_write_file_records(dev, a->file, (uint16_t)a->address, 1, record);
		else if (a->target == MOGBUS_TARGET_CANOPEN)
			(void)mogbus_device_write_canopen(dev, a->node, a->index, a->subindex, 0, 2, bytes);
	}
	return 0;
}

static mogbus_device_t*
target_device(mogbus_t** ctx)
{
	mogbus_device_t*	dev    = NULL;
	uint16_t		file1[] = { 10, 20, 30 };
	uint16_t		file2[] = { 40 };
	uint16_t		fifo[]  = { 1, 2, 3 };
	uint8_t			obj[]   = { 1, 2, 3, 4 };

	mogbus_create(ctx);
	mogbus_add_device(*ctx, 1, "targets", &dev);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 1, 0, 3, file1) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 2, 5, 1, file2) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_fifo(dev, 0x0100, 3, fifo) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_canopen_object(dev, 5, 0x6041, 0, 4, obj) == MOGBUS_OK);
	memset(&target_log, 0, sizeof(target_log));
	ASSERT_EQ(mogbus_device_set_access_hook(dev, target_hook, NULL) == MOGBUS_OK);
	return dev;
}

TEST(ApiHostFileRecordFifoAndCanopenFunctions)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev = NULL;
	mogbus_add_device(ctx, 1, "host", &dev);
	uint16_t	in[3]  = { 1, 2, 3 };
	uint16_t	out[3] = { 0 };
	uint8_t		bin[4] = { 9, 8, 7, 6 };
	uint8_t		bout[4] = { 0 };

	/* file records */
	ASSERT_EQ(mogbus_device_add_file_record(dev, 3, 10, 3, in) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 3, 11, 2, out) == MOGBUS_OK && out[0] == 2 && out[1] == 3);
	ASSERT_EQ(mogbus_device_write_file_records(dev, 3, 10, 2, in + 1) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 3, 10, 3, out) == MOGBUS_OK && out[0] == 2 && out[1] == 3 && out[2] == 3);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 3, 12, 2, in) != MOGBUS_OK);	/* overlap */
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "overlaps") != NULL);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 0, 0, 1, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 4, 9999, 2, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 4, 0, 0, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_file_record(dev, 4, 0, 1, NULL) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_file_record(NULL, 4, 0, 1, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 3, 12, 2, out) != MOGBUS_OK);	/* record 13 is undefined */
	ASSERT_EQ(mogbus_device_read_file_records(dev, 9, 0, 1, out) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_write_file_records(dev, 3, 12, 2, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 3, 12, 1, out) == MOGBUS_OK && out[0] == 3);	/* untouched */
	ASSERT_EQ(mogbus_device_read_file_records(dev, 3, 10, 0, out) != MOGBUS_OK);

	/* FIFO */
	ASSERT_EQ(mogbus_device_add_fifo(dev, 7, 0, NULL) == MOGBUS_OK);	/* empty queue */
	ASSERT_EQ(mogbus_device_set_fifo(dev, 7, 3, in) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_fifo(dev, 7, 1, in) != MOGBUS_OK);	/* duplicate */
	ASSERT_EQ(strstr(mogbus_last_error(ctx), "already defined") != NULL);
	ASSERT_EQ(mogbus_device_set_fifo(dev, 8, 1, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_fifo(dev, 7, 32, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_fifo(dev, 9, 32, in) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_fifo(dev, 7, 0, NULL) == MOGBUS_OK);

	/* CANopen */
	ASSERT_EQ(mogbus_device_add_canopen_object(dev, 5, 0x6041, 1, 4, bin) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_canopen(dev, 5, 0x6041, 1, 1, 2, bout) == MOGBUS_OK && bout[0] == 8 && bout[1] == 7);
	ASSERT_EQ(mogbus_device_write_canopen(dev, 5, 0x6041, 1, 2, 2, bin) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_canopen(dev, 5, 0x6041, 1, 0, 4, bout) == MOGBUS_OK);
	ASSERT_EQ(bout[0] == 9 && bout[1] == 8 && bout[2] == 9 && bout[3] == 8);
	ASSERT_EQ(mogbus_device_write_canopen(dev, 5, 0x6041, 1, 3, 2, bin) != MOGBUS_OK);	/* past the length */
	ASSERT_EQ(mogbus_device_read_canopen(dev, 5, 0x6041, 1, 0, 5, bout) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_canopen(dev, 5, 0x6041, 2, 0, 1, bout) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_canopen_object(dev, 5, 0x6041, 1, 4, bin) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_canopen_object(dev, 0, 0x6041, 2, 4, bin) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_canopen_object(dev, 128, 0x6041, 2, 4, bin) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_canopen_object(dev, 5, 0x6041, 2, 0, bin) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_canopen_object(dev, 5, 0x6041, 2, 239, bin) != MOGBUS_OK);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookFileRecordReadRunsPerSubRequestAndRefreshes)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = target_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_READ_FILE_RECORD, 14,
			  6, 0, 1, 0, 1, 0, 2,		/* file 1, records 1-2 */
			  6, 0, 2, 0, 5, 0, 1 };	/* file 2, record 5 */

	target_log.refresh = true;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 2 + 5 + 3 + 2);
	ASSERT_EQ(target_log.calls == 2);
	ASSERT_EQ(target_log.seen[0].target == MOGBUS_TARGET_FILE_RECORD &&
		  target_log.seen[0].kind == MOGBUS_ACCESS_READ &&
		  target_log.seen[0].file == 1 &&
		  target_log.seen[0].address == 1 &&
		  target_log.seen[0].count == 2 &&
		  target_log.seen[0].values == NULL &&
		  target_log.seen[0].function_code == FC_READ_FILE_RECORD);
	ASSERT_EQ(target_log.seen[1].file == 2 && target_log.seen[1].address == 5 && target_log.seen[1].count == 1);
	/* the hook rewrote record 1 and record 5 before the response was built */
	ASSERT_EQ(resp[0] == FC_READ_FILE_RECORD && resp[1] == 10);
	ASSERT_EQ(resp[2] == 5 && resp[3] == 6 && resp[4] == 0 && resp[5] == 99 && resp[6] == 0 && resp[7] == 30);
	ASSERT_EQ(resp[8] == 3 && resp[9] == 6 && resp[10] == 0 && resp[11] == 99);

	/* a veto on the second sub-request answers that exception */
	memset(&target_log, 0, sizeof(target_log));
	target_log.veto_call = 2;
	target_log.answer    = 6;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 2 && resp[0] == (FC_READ_FILE_RECORD | 0x80) && resp[1] == 6);
	ASSERT_EQ(target_log.calls == 2);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookFileRecordSkippedForInvalidRequests)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = target_device(&ctx);
	u8		resp[260];
	u8		bad_record[] = { FC_READ_FILE_RECORD, 14,
				 6, 0, 1, 0, 1, 0, 1,
				 6, 0, 1, 0, 3, 0, 1 };	/* file 1 has no record 3 */
	u8		bad_shape[] = { FC_READ_FILE_RECORD, 7, 5, 0, 1, 0, 0, 0, 1 };
	u8		bad_write[] = { FC_WRITE_FILE_RECORD, 18,
				6, 0, 1, 0, 0, 0, 1, 0, 7,
				6, 0, 1, 0, 3, 0, 1, 0, 8 };

	ASSERT_EQ(hook_pdu(dev, bad_record, sizeof(bad_record), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(hook_pdu(dev, bad_shape, sizeof(bad_shape), resp) == 2 && resp[1] == 3);
	ASSERT_EQ(hook_pdu(dev, bad_write, sizeof(bad_write), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(target_log.calls == 0);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookFileRecordWriteVetoWritesNothing)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = target_device(&ctx);
	u8		resp[260];
	u8		req[] = { FC_WRITE_FILE_RECORD, 18,
			  6, 0, 1, 0, 0, 0, 1, 0, 77,		/* file 1, record 0 = 77 */
			  6, 0, 1, 0, 1, 0, 1, 0, 88 };	/* file 1, record 1 = 88 */
	uint16_t	out[3];

	target_log.veto_call = 2;
	target_log.answer    = 4;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 2 && resp[0] == (FC_WRITE_FILE_RECORD | 0x80) && resp[1] == 4);
	ASSERT_EQ(target_log.calls == 2);
	ASSERT_EQ(target_log.seen[0].kind == MOGBUS_ACCESS_WRITE &&
		  target_log.seen[0].target == MOGBUS_TARGET_FILE_RECORD &&
		  target_log.seen[0].file == 1 &&
		  target_log.seen[0].address == 0 &&
		  target_log.seen[0].count == 1 &&
		  target_log.values[0][0] == 77);
	ASSERT_EQ(target_log.values[1][0] == 88 && target_log.seen[1].address == 1);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 1, 0, 3, out) == MOGBUS_OK);
	ASSERT_EQ(out[0] == 10 && out[1] == 20 && out[2] == 30);

	memset(&target_log, 0, sizeof(target_log));
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == sizeof(req) && memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(target_log.calls == 2);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 1, 0, 3, out) == MOGBUS_OK);
	ASSERT_EQ(out[0] == 77 && out[1] == 88 && out[2] == 30);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookFifoRefreshesBeforeTheRead)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = target_device(&ctx);
	u8		resp[260];
	u8		req[]     = { FC_READ_FIFO_QUEUE, 0x01, 0x00 };
	u8		missing[] = { FC_READ_FIFO_QUEUE, 0x02, 0x00 };

	ASSERT_EQ(hook_pdu(dev, missing, sizeof(missing), resp) == 2 && resp[1] == 2);
	ASSERT_EQ(target_log.calls == 0);

	target_log.refresh = true;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 5 + 4);
	ASSERT_EQ(target_log.calls == 1 &&
		  target_log.seen[0].target == MOGBUS_TARGET_FIFO &&
		  target_log.seen[0].kind == MOGBUS_ACCESS_READ &&
		  target_log.seen[0].address == 0x0100 &&
		  target_log.seen[0].function_code == FC_READ_FIFO_QUEUE);
	ASSERT_EQ(resp[3] == 0 && resp[4] == 2 && resp[6] == 7 && resp[8] == 8);

	memset(&target_log, 0, sizeof(target_log));
	target_log.veto_call = 1;
	target_log.answer    = 6;
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 2 && resp[0] == (FC_READ_FIFO_QUEUE | 0x80) && resp[1] == 6);
	mogbus_destroy(ctx);
}

TEST(ApiAccessHookCanopenReadRefreshesAndWriteVetoUsesAbortCode)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = target_device(&ctx);
	u8		resp[260];
	u8		read[]  = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x00, 0x00, 5, 0x60, 0x41, 0,
				    0, 0, 0, 4 };
	u8		write[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x01, 0x00, 5, 0x60, 0x41, 0,
				    0, 1, 0, 2, 0x11, 0x22 };
	u8		missing[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x00, 0x00, 5, 0x60, 0x42, 0,
				      0, 0, 0, 1 };
	u8		out[4];

	ASSERT_EQ(hook_pdu(dev, missing, sizeof(missing), resp) == 10 && resp[6] == 0x0B);
	ASSERT_EQ(target_log.calls == 0);

	target_log.refresh = true;
	ASSERT_EQ(hook_pdu(dev, read, sizeof(read), resp) == 12 + 4);
	ASSERT_EQ(target_log.calls == 1 &&
		  target_log.seen[0].target == MOGBUS_TARGET_CANOPEN &&
		  target_log.seen[0].kind == MOGBUS_ACCESS_READ &&
		  target_log.seen[0].node == 5 &&
		  target_log.seen[0].index == 0x6041 &&
		  target_log.seen[0].subindex == 0 &&
		  target_log.seen[0].address == 0 &&
		  target_log.seen[0].count == 4 &&
		  target_log.seen[0].data == NULL &&
		  target_log.seen[0].function_code == FC_ENCAPSULATED_INTERFACE_TRANSPORT);
	ASSERT_EQ(resp[12] == 0xAB && resp[13] == 0xCD && resp[14] == 3 && resp[15] == 4);

	memset(&target_log, 0, sizeof(target_log));
	target_log.veto_call = 1;
	target_log.answer    = 6;
	ASSERT_EQ(hook_pdu(dev, write, sizeof(write), resp) == 10);
	{
		const u8 want[] = { 0xAB, 0xFF, 0x00, 0x06, 0x0D, 0xCE, 0x20, 0x00, 0x00, 0x08 };

		ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	}
	ASSERT_EQ(target_log.seen[0].kind == MOGBUS_ACCESS_WRITE &&
		  target_log.seen[0].address == 1 &&
		  target_log.seen[0].count == 2 &&
		  target_log.seen[0].data_len == 2 &&
		  target_log.data[0][0] == 0x11 &&
		  target_log.data[0][1] == 0x22);
	ASSERT_EQ(mogbus_device_read_canopen(dev, 5, 0x6041, 0, 0, 4, out) == MOGBUS_OK);
	ASSERT_EQ(out[0] == 0xAB && out[1] == 0xCD && out[2] == 3 && out[3] == 4);	/* the read refresh, not the vetoed write */

	memset(&target_log, 0, sizeof(target_log));
	ASSERT_EQ(hook_pdu(dev, write, sizeof(write), resp) == 12);
	ASSERT_EQ(mogbus_device_read_canopen(dev, 5, 0x6041, 0, 0, 4, out) == MOGBUS_OK);
	ASSERT_EQ(out[0] == 0xAB && out[1] == 0x11 && out[2] == 0x22 && out[3] == 4);
	mogbus_destroy(ctx);
}

TEST(ApiHostCallsNeverRunTheTargetHook)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = target_device(&ctx);
	uint16_t	word = 1;
	uint8_t		byte = 1;

	ASSERT_EQ(mogbus_device_write_file_records(dev, 1, 0, 1, &word) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_file_records(dev, 1, 0, 1, &word) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_set_fifo(dev, 0x0100, 1, &word) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_write_canopen(dev, 5, 0x6041, 0, 0, 1, &byte) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_read_canopen(dev, 5, 0x6041, 0, 0, 1, &byte) == MOGBUS_OK);
	ASSERT_EQ(target_log.calls == 0);
	mogbus_destroy(ctx);
}

/* ---- Programmable user-defined function codes ---- */

static struct function_log
{
	int		calls;
	uint8_t		unit;
	uint8_t		code;
	size_t		len;
	uint8_t		data[8];
	int		result;
	size_t		reply_len;
} fn_log;

/* Replies with the request data reversed, plus a live holding value; the first data byte picks
 * the other outcomes. */
static int
station_fn(void* user, mogbus_device_t* dev, uint8_t unit, uint8_t code, const uint8_t* data,
	   size_t len, uint8_t* reply, size_t reply_cap, size_t* reply_len)
{
	double v = 0;

	fn_log.calls++;
	fn_log.unit = unit;
	fn_log.code = code;
	fn_log.len  = len;
	memcpy(fn_log.data, data, len < sizeof(fn_log.data) ? len : sizeof(fn_log.data));
	if (user)
		(*(int*)user)++;
	if (len > 0 && data[0] == 0xE0)
		return data[1];
	if (len > 0 && data[0] == 0xE1)
		return MOGBUS_FUNCTION_NO_RESPONSE;
	if (len > 0 && data[0] == 0xE2)
	{
		memset(reply, 0xAB, reply_cap);
		*reply_len = reply_cap;
		return 0;
	}
	if (len > 0 && data[0] == 0xE3)
	{
		*reply_len = reply_cap + 1;
		return 0;
	}
	if (len > 0 && data[0] == 0xE4)
		return 300;
	(void)mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v);
	for (size_t i = 0; i < len; i++)
		reply[i] = data[len - 1 - i];
	reply[len]     = (uint8_t)v;
	*reply_len     = len + 1;
	return fn_log.result;
}

static mogbus_device_t*
function_device(mogbus_t** ctx, int* user)
{
	mogbus_device_t* dev = NULL;
	mogbus_point_t	p = holding(0, 7);

	mogbus_create(ctx);
	mogbus_add_device(*ctx, 1, "fn", &dev);
	(void)mogbus_device_add_point(dev, &p);
	memset(&fn_log, 0, sizeof(fn_log));
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0x41, station_fn, user) == MOGBUS_OK);
	return dev;
}

TEST(ApiFunctionCodeAnswersWithTheCallbackReply)
{
	mogbus_t*	ctx = NULL;
	int		count = 0;
	mogbus_device_t* dev = function_device(&ctx, &count);
	u8		resp[MODBUS_MAX_PDU];
	const u8	req[] = { 0x41, 1, 2, 3 };
	mogbus_device_info_t info;

	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[0] == 0x41 && resp[1] == 3 && resp[2] == 2 && resp[3] == 1 && resp[4] == 7);
	ASSERT_EQ(count == 1 && fn_log.code == 0x41 && fn_log.len == 3 && fn_log.data[0] == 1);
	mogbus_device_set_value(dev, MOGBUS_HOLDING, 0, 9);
	ASSERT_EQ(hook_pdu(dev, req, sizeof(req), resp) == 5 && resp[4] == 9);
	ASSERT_EQ(mogbus_device_info(dev, &info));
	ASSERT_EQ(info.custom_fc_count == 1 && info.custom_fcs[0].code == 0x41 && info.custom_fcs[0].name[0] == 0);
	ASSERT_EQ(((device_t*)dev)->message_count == 2 && ((device_t*)dev)->com_event_count == 2);
	mogbus_destroy(ctx);
}

TEST(ApiFunctionCodeExceptionNoResponseAndBrokenCallbacks)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = function_device(&ctx, NULL);
	u8		resp[MODBUS_MAX_PDU];
	const u8	exc[]   = { 0x41, 0xE0, 6 };
	const u8	silent[] = { 0x41, 0xE1 };
	const u8	full[]  = { 0x41, 0xE2 };
	const u8	over[]  = { 0x41, 0xE3 };
	const u8	bad[]   = { 0x41, 0xE4 };
	const u8	empty[] = { 0x41 };

	ASSERT_EQ(hook_pdu(dev, exc, sizeof(exc), resp) == 2);
	ASSERT_EQ(resp[0] == 0xC1 && resp[1] == 6);
	ASSERT_EQ(((device_t*)dev)->diag_exception_count == 1);
	ASSERT_EQ(hook_pdu(dev, silent, sizeof(silent), resp) == 0);
	ASSERT_EQ(hook_pdu(dev, full, sizeof(full), resp) == MODBUS_MAX_PDU);
	ASSERT_EQ(resp[0] == 0x41 && resp[1] == 0xAB && resp[252] == 0xAB);
	ASSERT_EQ(hook_pdu(dev, over, sizeof(over), resp) == 2);	/* too long: exception 04 */
	ASSERT_EQ(resp[0] == 0xC1 && resp[1] == 4);
	ASSERT_EQ(hook_pdu(dev, bad, sizeof(bad), resp) == 2);	/* result 300 */
	ASSERT_EQ(resp[0] == 0xC1 && resp[1] == 4);
	ASSERT_EQ(hook_pdu(dev, empty, sizeof(empty), resp) == 2);
	ASSERT_EQ(resp[0] == 0x41 && resp[1] == 7 && fn_log.len == 0);
	mogbus_destroy(ctx);
}

static int
noop_fn(void* user, mogbus_device_t* dev, uint8_t unit, uint8_t code, const uint8_t* data,
	size_t len, uint8_t* reply, size_t reply_cap, size_t* reply_len)
{
	(void)user; (void)dev; (void)unit; (void)code; (void)data; (void)len; (void)reply; (void)reply_cap;
	*reply_len = 0;
	return 0;
}

TEST(ApiFunctionCodeRejectsBuiltinDuplicatesAndTheLimit)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev = function_device(&ctx, NULL);

	ASSERT_EQ(mogbus_device_add_function_code(NULL, 0x42, noop_fn, NULL) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0x42, NULL, NULL) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0, noop_fn, NULL) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_function_code(dev, 128, noop_fn, NULL) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0x03, noop_fn, NULL) != MOGBUS_OK);	/* built in */
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0x2B, noop_fn, NULL) != MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0x41, noop_fn, NULL) != MOGBUS_OK);	/* duplicate */
	for (uint8_t c = 0x42; c <= 0x48; c++)
		ASSERT_EQ(mogbus_device_add_function_code(dev, c, noop_fn, NULL) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0x49, noop_fn, NULL) != MOGBUS_OK);	/* ninth */
	mogbus_destroy(ctx);
}

#if WITH_CONFIG
TEST(ApiFunctionCodeConflictsWithConfigAndSharesFaultsAndListenOnly)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* dev = NULL;
	mogbus_device_info_t info;
	u8		resp[MODBUS_MAX_PDU];
	const u8	req[] = { 0x41, 5 };
	const u8	cfg[] = { 0x42, 1 };
	fault_decision_t decision;
	rng_t		rng;

	ASSERT_EQ(write_config(path,
		"(device (unit-id 1) (holding 0 (static 7))\n"
		"  (function-code 66 (name \"cfg\") (on \"01\" (reply \"AA\")))\n"
		"  (fault (kind exception) (code 6) (percent 100)))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	dev = mogbus_find_device(ctx, 1);
	ASSERT_EQ(mogbus_device_add_function_code(dev, 66, noop_fn, NULL) != MOGBUS_OK);	/* defined by config */
	ASSERT_EQ(mogbus_device_add_function_code(dev, 0x41, station_fn, NULL) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_info(dev, &info) && info.custom_fc_count == 2);
	ASSERT_EQ(info.custom_fcs[0].code == 66 && strcmp(info.custom_fcs[0].name, "cfg") == 0);
	ASSERT_EQ(info.custom_fcs[1].code == 0x41 && info.custom_fcs[1].name[0] == 0);
	rng_seed(&rng, 1);
	ASSERT_EQ(modbus_handle_request((device_t*)dev, &rng, 0, req, sizeof(req), resp, &decision) > 0);
	ASSERT_EQ(decision.exception_code == 6);	/* the fault still replaces the answer */
	ASSERT_EQ(modbus_handle_request((device_t*)dev, &rng, 0, cfg, sizeof(cfg), resp, &decision) == 2);
	ASSERT_EQ(decision.exception_code == 6);
	/* listen-only: no call, no response */
	{
		const u8 listen[] = { 0x08, 0x00, 0x04, 0x00, 0x00 };
		int	 before   = fn_log.calls;

		ASSERT_EQ(modbus_handle_pdu((device_t*)dev, 0, listen, sizeof(listen), resp) == 0);
		ASSERT_EQ(modbus_handle_pdu((device_t*)dev, 0, req, sizeof(req), resp) == 0);
		ASSERT_EQ(fn_log.calls == before);
	}
	mogbus_destroy(ctx);
	unlink(path);
}
#endif

/* ---- Frame hook ---- */

enum frame_action_t
{
	FRAME_PASS,
	FRAME_EDIT,	/* decode the PDU, set pdu[edit_index] = edit_value, encode again */
	FRAME_DROP,
	FRAME_TOO_LONG,
	FRAME_BAD_CHECKSUM,
};

static struct frame_log_t
{
	unsigned		calls;
	mogbus_frame_direction_t dir[8];
	mogbus_mode_t		mode;
	mogbus_server_t*	server;
	char			peer[64];
	size_t			len[8];
	size_t			capacity;
	struct frame_rule_t
	{
		enum frame_action_t	action;
		unsigned		edit_index;
		uint8_t			edit_value;
	} rule[2];	/* by direction */
	unsigned		long_warnings;
} frame_log;

static void
frame_warn_sink(void* user, mogbus_log_level_t level, const char* message)
{
	(void)user;
	if (level == MOGBUS_LOG_WARN && strstr(message, "frame hook returned"))
		frame_log.long_warnings++;
}

/* Rewrites the PDU of a frame in any framing and encodes the frame again. */
static size_t
frame_edit_pdu(mogbus_frame_t* f, unsigned index, uint8_t value)
{
	u8		pdu[MODBUS_MAX_PDU];
	u8		decoded[MODBUS_MAX_PDU + 2];
	u8		addr = 0;
	const u8*	in = NULL;
	u32		n = 0;

	switch (f->mode)
	{
	case MOGBUS_MODE_TCP:
	case MOGBUS_MODE_UDP:
		f->data[7 + index] = value;
		return f->len;
	case MOGBUS_MODE_RTU_TCP:
	case MOGBUS_MODE_RTU_UDP:
	case MOGBUS_MODE_RTU_FD:
		if (!modbus_rtu_decode(f->data, (u32)f->len, &addr, &in, &n))
			return 0;
		memcpy(pdu, in, n);
		pdu[index] = value;
		return modbus_rtu_encode(f->data, addr, pdu, n);
	default:
		if (!modbus_ascii_decode(f->data, (u32)f->len, decoded, &addr, &in, &n))
			return 0;
		memcpy(pdu, in, n);
		pdu[index] = value;
		return modbus_ascii_encode(f->data, addr, pdu, n);
	}
}

static size_t
frame_hook(void* user, mogbus_server_t* server, mogbus_frame_t* f)
{
	const struct frame_rule_t* rule = &frame_log.rule[f->direction];

	(void)user;
	if (frame_log.calls < 8)
	{
		frame_log.dir[frame_log.calls] = f->direction;
		frame_log.len[frame_log.calls] = f->len;
	}
	frame_log.calls++;
	frame_log.mode     = f->mode;
	frame_log.server   = server;
	frame_log.capacity = f->capacity;
	snprintf(frame_log.peer, sizeof(frame_log.peer), "%s", f->peer);
	switch (rule->action)
	{
	case FRAME_EDIT:
		return frame_edit_pdu(f, rule->edit_index, rule->edit_value);
	case FRAME_DROP:
		return 0;
	case FRAME_TOO_LONG:
		return f->capacity + 1;
	case FRAME_BAD_CHECKSUM:
		if (f->mode == MOGBUS_MODE_ASCII_TCP || f->mode == MOGBUS_MODE_ASCII_UDP ||
		    f->mode == MOGBUS_MODE_ASCII_FD)
			f->data[f->len - 4] = f->data[f->len - 4] == '0' ? '1' : '0';
		else
			f->data[f->len - 1] ^= 0xFF;
		return f->len;
	default:
		return f->len;
	}
}

/* Sends the read of holding register 0 of unit 1 in the framing of the mode. */
static bool
frame_send(mogbus_mode_t mode, int fd)
{
	switch (mode)
	{
	case MOGBUS_MODE_TCP:
	case MOGBUS_MODE_UDP:
		return ts_mbap_send(fd, 1, 7, READ_HOLDING, sizeof(READ_HOLDING));
	case MOGBUS_MODE_RTU_TCP:
	case MOGBUS_MODE_RTU_UDP:
	case MOGBUS_MODE_RTU_FD:
		return ts_rtu_send(fd, 1, READ_HOLDING, sizeof(READ_HOLDING), true);
	default:
		return ts_ascii_send(fd, 1, READ_HOLDING, sizeof(READ_HOLDING), true);
	}
}

/* Sends the request, steps the server and returns the register value of the answer, or -1. */
static int
frame_roundtrip(mogbus_server_t* server, mogbus_mode_t mode, int fd)
{
	u8		raw[MODBUS_ASCII_MAX_FRAME];
	u8		decoded[MODBUS_MAX_PDU + 2];
	u8		pdu[MODBUS_MAX_PDU];
	u8		addr = 0;
	const u8*	in = pdu;
	u32		n = 0;
	s32		got = 0;
	bool		ok = frame_send(mode, fd) && pump(server, fd);

	if (!ok)
		return -1;
	switch (mode)
	{
	case MOGBUS_MODE_TCP:
		ok = ts_mbap_recv(fd, 1, 7, pdu, &n, 1000);
		break;
	case MOGBUS_MODE_UDP:
		ok = ts_udp_mbap_recv(fd, 1, 7, pdu, &n, 1000);
		break;
	case MOGBUS_MODE_RTU_TCP:
	case MOGBUS_MODE_RTU_FD:
		ok = ts_rtu_recv(fd, &addr, pdu, &n, 1000, true);
		break;
	case MOGBUS_MODE_ASCII_TCP:
	case MOGBUS_MODE_ASCII_FD:
		ok = ts_ascii_recv_response(fd, &addr, pdu, &n, 1000, true);
		break;
	default:
		got = ts_udp_recv(fd, raw, sizeof(raw), 1000);
		ok = got > 0;
		if (ok && mode == MOGBUS_MODE_RTU_UDP)
			ok = modbus_rtu_decode(raw, (u32)got, &addr, &in, &n);
		else if (ok)
			ok = modbus_ascii_decode(raw, (u32)got, decoded, &addr, &in, &n);
		break;
	}
	if (!ok || n != 4)
		return -1;
	return (in[2] << 8) | in[3];
}

/* Steps the server for a while and reports whether the peer got nothing. */
static bool
frame_silent(mogbus_server_t* server, int fd)
{
	u64 until = os_now_ms() + 150;

	while (os_now_ms() < until)
		(void)mogbus_server_step(server, 1);
	return ts_no_data(fd, 50);
}

TEST(ApiFrameHookSeesRewritesAndDropsFramesOnEveryTransport)
{
	static const struct
	{
		mogbus_mode_t	mode;
		const char*	name;
		bool		datagram;
		bool		fd;
		size_t		capacity;
		size_t		req_len;
		size_t		resp_len;
	} modes[] = {
		{ MOGBUS_MODE_TCP,       "tcp",       false, false, 260, 12, 11 },
		{ MOGBUS_MODE_UDP,       "udp",       true,  false, 260, 12, 11 },
		{ MOGBUS_MODE_RTU_TCP,   "rtu-tcp",   false, false, 256, 8,  7 },
		{ MOGBUS_MODE_RTU_UDP,   "rtu-udp",   true,  false, 256, 8,  7 },
		{ MOGBUS_MODE_RTU_FD,    "rtu-fd",    false, true,  256, 8,  7 },
		{ MOGBUS_MODE_ASCII_TCP, "ascii-tcp", false, false, 513, 17, 15 },
		{ MOGBUS_MODE_ASCII_UDP, "ascii-udp", true,  false, 513, 17, 15 },
		{ MOGBUS_MODE_ASCII_FD,  "ascii-fd",  false, true,  513, 17, 15 },
	};
	static const struct
	{
		const char*		name;
		struct frame_rule_t	rule[2];
		int			value;		/* answer, or -1 for none */
		unsigned		calls;
		bool			warns;
		bool			checksum;	/* frames with a checksum only */
	} cases[] = {
		{ "pass",        { { .action = FRAME_PASS }, { .action = FRAME_PASS } },                  0x1111, 2, false, false },
		{ "rx-edit",     { { FRAME_EDIT, 2, 1 }, { .action = FRAME_PASS } },            0x2222, 2, false, false },
		{ "tx-edit",     { { .action = FRAME_PASS }, { .action = FRAME_EDIT, 3, 0x99 } },         0x1199, 2, false, false },
		{ "rx-drop",     { { .action = FRAME_DROP }, { .action = FRAME_PASS } },                  -1,     1, false, false },
		{ "tx-drop",     { { .action = FRAME_PASS }, { .action = FRAME_DROP } },                  -1,     2, false, false },
		{ "rx-too-long", { { .action = FRAME_TOO_LONG }, { .action = FRAME_PASS } },              -1,     1, true,  false },
		{ "tx-too-long", { { .action = FRAME_PASS }, { .action = FRAME_TOO_LONG } },              -1,     2, true,  false },
		{ "rx-bad-sum",  { { .action = FRAME_BAD_CHECKSUM }, { .action = FRAME_PASS } },          -1,     1, false, true  },
	};
	char name[64];

	for (u32 m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
	{
		for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
		{
			mogbus_t*	ctx = NULL;
			mogbus_create(&ctx);
			mogbus_device_t* dev = NULL;
			mogbus_add_device(ctx, 1, "frames", &dev);
			mogbus_point_t	p0 = holding(0, 0x1111);
			mogbus_point_t	p1 = holding(1, 0x2222);
			mogbus_server_options_t o = mogbus_server_options_defaults();
			mogbus_server_t* server;
			int		pair[2] = { -1, -1 };
			int		fd;
			unsigned	calls;

			if (cases[c].checksum && (modes[m].mode == MOGBUS_MODE_TCP || modes[m].mode == MOGBUS_MODE_UDP))
				continue;
			snprintf(name, sizeof(name), "%s %s", modes[m].name, cases[c].name);
			test_case = name;
			memset(&frame_log, 0, sizeof(frame_log));
			memcpy(frame_log.rule, cases[c].rule, sizeof(frame_log.rule));
			ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p0) == MOGBUS_OK &&
				  mogbus_device_add_point(dev, &p1) == MOGBUS_OK);
			o.mode = modes[m].mode;
			o.frame_gap_ms = 2;
			if (modes[m].fd)
			{
				ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
				o.fd = pair[1];
				fd = pair[0];
			}
			else
				o.port = ts_free_port();
			mogbus_server_create(ctx, &o, &server);
			ASSERT_EQ(server != NULL);
			if (!modes[m].fd)
			{
				fd = modes[m].datagram ? ts_udp_loopback(o.port, AF_INET, 1000) :
							 ts_connect_loopback(o.port, AF_INET, 1000);
				ASSERT_EQ(fd >= 0);
			}
			ASSERT_EQ(mogbus_server_set_frame_hook(NULL, frame_hook, &frame_log) != MOGBUS_OK);
			ASSERT_EQ(mogbus_server_set_frame_hook(server, frame_hook, &frame_log) == MOGBUS_OK);
			mogbus_set_log_sink(frame_warn_sink, NULL);

			if (cases[c].value >= 0)
				ASSERT_EQ(frame_roundtrip(server, modes[m].mode, fd) == cases[c].value);
			else
			{
				ASSERT_EQ(frame_send(modes[m].mode, fd));
				ASSERT_EQ(frame_silent(server, fd));
			}
			ASSERT_EQ(frame_log.calls == cases[c].calls);
			ASSERT_EQ(frame_log.dir[0] == MOGBUS_FRAME_RECEIVED);
			ASSERT_EQ(cases[c].calls < 2 || frame_log.dir[1] == MOGBUS_FRAME_SENT);
			ASSERT_EQ(frame_log.mode == modes[m].mode && frame_log.server == server);
			ASSERT_EQ(frame_log.capacity == modes[m].capacity);
			ASSERT_EQ(frame_log.len[0] == modes[m].req_len ||
				  cases[c].rule[0].action == FRAME_TOO_LONG);
			ASSERT_EQ(cases[c].calls < 2 || frame_log.len[1] == modes[m].resp_len);
			if (modes[m].fd)
				ASSERT_EQ(frame_log.peer[0] == 0);
			else
				ASSERT_EQ(strncmp(frame_log.peer, "127.0.0.1:", 10) == 0);
			ASSERT_EQ(frame_log.long_warnings == (cases[c].warns ? 1u : 0u));

			/* The connection and the server are fine afterwards; removing the hook stops the calls. */
			calls = frame_log.calls;
			ASSERT_EQ(mogbus_server_set_frame_hook(server, NULL, NULL) == MOGBUS_OK);
			ASSERT_EQ(frame_roundtrip(server, modes[m].mode, fd) == 0x1111);
			ASSERT_EQ(frame_log.calls == calls);

			mogbus_set_log_sink(NULL, NULL);
			close(fd);
			mogbus_server_close(server);
			if (modes[m].fd)
				close(pair[1]);
			mogbus_destroy(ctx);
		}
	}
	test_case = 0;
}

/* Steps every server of the context until fd is readable. */
static bool
pump_context(mogbus_t* ctx, int fd)
{
	u64 deadline = os_now_ms() + 2000;

	while (os_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		(void)mogbus_step(ctx, 1);
		if (poll(&pfd, 1, 0) > 0)
			return true;
	}
	return false;
}

/* One FC 03 read of holding register 0 over MBAP; returns the value, -1 for no reply and -2
 * (exception code in *exception) for an exception. */
static int
mbap_read(mogbus_t* ctx, int fd, bool udp, u8 unit, u8* exception)
{
	u8	pdu[MODBUS_MAX_PDU];
	u32	len = 0;
	u16	tid = ts_next_tid();

	if (!ts_mbap_send(fd, unit, tid, READ_HOLDING, sizeof(READ_HOLDING)) ||
	    !pump_context(ctx, fd) ||
	    !(udp ? ts_udp_mbap_recv(fd, unit, tid, pdu, &len, 1000) :
		    ts_mbap_recv(fd, unit, tid, pdu, &len, 1000)))
		return -1;
	if (pdu[0] & 0x80)
	{
		*exception = pdu[1];
		return -2;
	}
	return (pdu[2] << 8) | pdu[3];
}

TEST(ApiServersServeTheirAssignedDevicesTogether)
{
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_device_t* one;
	mogbus_device_t* two;
	mogbus_point_t	p1 = holding(0, 11);
	mogbus_point_t	p2 = holding(0, 22);
	mogbus_server_options_t a = mogbus_server_options_defaults();
	mogbus_server_options_t b = mogbus_server_options_defaults();
	mogbus_server_options_t none = mogbus_server_options_defaults();
	mogbus_server_t* sa;
	mogbus_server_t* sb;
	u8		exception = 0;
	int		tcp;
	int		udp;

	ASSERT_EQ(ctx != NULL);
	mogbus_add_device(ctx, 1, "one", &one);
	mogbus_add_device(ctx, 2, "two", &two);
	ASSERT_EQ(one != NULL && two != NULL);
	ASSERT_EQ(mogbus_device_add_point(one, &p1) == MOGBUS_OK && mogbus_device_add_point(two, &p2) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_server_at(one, 0) == NULL);

	/* Bad assignments. */
	ASSERT_EQ(mogbus_device_add_server(one, "") == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_device_add_server(one, "0123456789012345678901234567890123456789") == MOGBUS_ERR_INVALID &&
		  strstr(mogbus_last_error(ctx), "too long") != NULL);

	ASSERT_EQ(mogbus_device_add_server(one, "tcp") == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_server(one, "tcp") == MOGBUS_OK);	/* twice is fine */
	ASSERT_EQ(mogbus_device_add_server(two, "udp") == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_server(one, "udp") == MOGBUS_OK);	/* served by both */
	ASSERT_EQ(strcmp(mogbus_device_server_at(one, 0), "tcp") == 0 &&
		  strcmp(mogbus_device_server_at(one, 1), "udp") == 0 &&
		  mogbus_device_server_at(one, 2) == NULL);

	/* A second unit 1 may exist, on another server only. */
	{
		mogbus_device_t* dup = NULL;
		mogbus_add_device(ctx, 1, "dup", &dup);

		ASSERT_EQ(dup != NULL);
		ASSERT_EQ(mogbus_device_add_server(dup, "tcp") == MOGBUS_ERR_INVALID &&
			  strstr(mogbus_last_error(ctx), "unit id 1") != NULL);
		ASSERT_EQ(mogbus_device_add_server(dup, "other") == MOGBUS_OK);
	}

	a.id   = "tcp";
	a.port = ts_free_port();
	b.id   = "udp";
	b.mode = MOGBUS_MODE_UDP;
	b.port = ts_free_port();

	/* A server needs devices that list its id; a server without id serves the unassigned ones. */
	none.id   = "nobody";
	none.port = ts_free_port();
	ASSERT_EQ(mogbus_server_create(ctx, &none, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "nobody") != NULL);
	none.id = NULL;
	ASSERT_EQ(mogbus_server_create(ctx, &none, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_server_check(ctx, &none) == MOGBUS_ERR_INVALID);
	none.id = "";
	ASSERT_EQ(mogbus_server_create(ctx, &none, &(mogbus_server_t*){ NULL }) != MOGBUS_OK);

	ASSERT_EQ(mogbus_step(ctx, 0) == MOGBUS_STEP_DONE);	/* no server yet */
	mogbus_server_create(ctx, &a, &sa);
	mogbus_server_create(ctx, &b, &sb);
	ASSERT_EQ(sa != NULL && sb != NULL);
	ASSERT_EQ(mogbus_server_create(ctx, &a, &(mogbus_server_t*){ NULL }) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "already exists") != NULL);

	tcp = ts_connect_loopback(a.port, AF_INET, 1000);
	udp = ts_udp_loopback(b.port, AF_INET, 1000);
	ASSERT_EQ(tcp >= 0 && udp >= 0);

	/* One mogbus_step() serves both transports; unit 1 is on both, unit 2 only on UDP. */
	ASSERT_EQ(mbap_read(ctx, tcp, false, 1, &exception) == 11);
	ASSERT_EQ(mbap_read(ctx, udp, true, 1, &exception) == 11);
	ASSERT_EQ(mbap_read(ctx, udp, true, 2, &exception) == 22);
	ASSERT_EQ(mbap_read(ctx, tcp, false, 2, &exception) == -2 && exception == 0x0B);

	/* The shared device shows a host write on both transports. */
	ASSERT_EQ(mogbus_device_set_value(one, MOGBUS_HOLDING, 0, 33) == MOGBUS_OK);
	ASSERT_EQ(mbap_read(ctx, tcp, false, 1, &exception) == 33);
	ASSERT_EQ(mbap_read(ctx, udp, true, 1, &exception) == 33);

	/* A server closed on its own leaves the others running; mogbus_stop() ends the combined step. */
	ASSERT_EQ(mogbus_step(ctx, 0) == MOGBUS_STEP_CONTINUE);
	mogbus_server_close(sb);
	ASSERT_EQ(mbap_read(ctx, tcp, false, 1, &exception) == 33);
	mogbus_stop(ctx);
	ASSERT_EQ(mogbus_step(ctx, 0) == MOGBUS_STEP_DONE);
	ASSERT_EQ(mogbus_run(ctx) == 0);
	close(tcp);
	close(udp);
	mogbus_destroy(ctx);
}

#if WITH_CONFIG
TEST(ApiLoadsSeveralServersFromAConfig)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	char		text[1024];
	int		cfd = mkstemp(path);
	mogbus_t*	ctx = NULL;
	mogbus_create(&ctx);
	mogbus_server_options_t o;
	mogbus_server_t* s0;
	mogbus_server_t* s1;
	u16		tcp_port = ts_free_port();
	u16		udp_port = ts_free_port();
	u8		exception = 0;
	int		tcp;
	int		udp;

	ASSERT_EQ(cfd >= 0 && ctx != NULL);
	ASSERT_EQ(mogbus_config_server_count(ctx) == 0);
	snprintf(text, sizeof(text),
		 "(server (id \"t\") (mode tcp) (port %u))\n"
		 "(server (id \"u\") (mode udp) (port %u))\n"
		 "(device (name \"a\") (unit-id 1) (servers \"t\") (holding 0 (static 5)))\n"
		 "(device (name \"b\") (unit-id 1) (servers \"u\") (holding 0 (static 6)))\n",
		 tcp_port, udp_port);
	ASSERT_EQ(write(cfd, text, strlen(text)) == (ssize_t)strlen(text));
	close(cfd);

	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	ASSERT_EQ(mogbus_config_server_count(ctx) == 2);
	ASSERT_EQ(mogbus_config_server_options_at(ctx, 1, &o) == MOGBUS_OK && strcmp(o.id, "u") == 0 &&
		  o.mode == MOGBUS_MODE_UDP && o.port == udp_port);
	ASSERT_EQ(mogbus_config_server_options(ctx, &o) == MOGBUS_OK && strcmp(o.id, "t") == 0);
	ASSERT_EQ(mogbus_config_server_options_at(ctx, 2, &o) != MOGBUS_OK);
	ASSERT_EQ(mogbus_server_create_from_config_at(ctx, 2, &(mogbus_server_t*){ NULL }) == MOGBUS_ERR_INVALID);

	/* The same unit id on different servers coexists; loading the file again would not. */
	ASSERT_EQ(mogbus_device_count(ctx) == 2);
	ASSERT_EQ(mogbus_load_config(ctx, path) != MOGBUS_OK && strstr(mogbus_last_error(ctx), "unit id 1") != NULL);
	ASSERT_EQ(mogbus_device_count(ctx) == 2);

	mogbus_server_create_from_config(ctx, &s0);
	mogbus_server_create_from_config_at(ctx, 1, &s1);
	ASSERT_EQ(s0 != NULL && s1 != NULL);
	tcp = ts_connect_loopback(tcp_port, AF_INET, 1000);
	udp = ts_udp_loopback(udp_port, AF_INET, 1000);
	ASSERT_EQ(tcp >= 0 && udp >= 0);
	ASSERT_EQ(mbap_read(ctx, tcp, false, 1, &exception) == 5);
	ASSERT_EQ(mbap_read(ctx, udp, true, 1, &exception) == 6);
	close(tcp);
	close(udp);
	mogbus_destroy(ctx);
	unlink(path);
}
#endif
