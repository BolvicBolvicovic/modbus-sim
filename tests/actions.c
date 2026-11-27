#include "vtest.h"
#include "config/config.h"
#include "device/action.h"
#include "modbus/pdu.h"
#include "mogbus.h"
#include "vlib/macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Writes `text` to a temp file and loads it; the arenas outlive the test on purpose. */
static bool
load_text(const char* text, config_t* config, char* err, u32 err_size)
{
	char		path[]  = "/tmp/mogbus-actions-XXXXXX";
	arena_t*	persist = ARENA_ALLOC();
	arena_t*	scratch = ARENA_ALLOC();
	int		fd      = mkstemp(path);
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

static device_t*
load_device(const char* text)
{
	static config_t	c;
	char		e[1024] = "";
	bool		ok = false;

	memset(&c, 0, sizeof(c));
	ok = load_text(text, &c, e, sizeof(e));
	if (!ok)
		fprintf(stderr, "config failed to load: %s\n  got: %s\n", text, e);
	ASSERT_EQ(ok);
	return c.devices[0];
}

#define EXPECT_ERROR(text, fragment)							\
	do {										\
		config_t	c = { 0 };						\
		char		e[1024] = "";						\
		ASSERT_FAILURE(load_text((text), &c, e, sizeof(e)));			\
		if (strstr(e, (fragment)) == NULL)					\
			fprintf(stderr, "wanted '%s' in: %s\n", (fragment), e);		\
		ASSERT_EQ(strstr(e, (fragment)) != NULL);				\
	} while (0)

#define SERVER "(server (mode tcp) (port 15021))\n"

/* Sends one request; returns the response length. */
static u32
request(device_t* dev, u64 now_ms, const u8* req, u32 len, u8* resp)
{
	return modbus_handle_pdu(dev, now_ms, req, len, resp);
}

/* FC `fc` read of `qty` items at `address`; returns 0 and the first value, or the exception code. */
static u8
read_first(device_t* dev, u64 now_ms, u8 fc, u16 address, u16 qty, u16* value)
{
	u8	req[5] = { fc, (u8)(address >> 8), (u8)address, (u8)(qty >> 8), (u8)qty };
	u8	resp[260];

	request(dev, now_ms, req, 5, resp);
	if (resp[0] & 0x80)
		return resp[1];
	if (value)
		*value = fc <= 2 ? (u16)(resp[2] & 1) : (u16)((resp[2] << 8) | resp[3]);
	return 0;
}

static u8
write_single(device_t* dev, u64 now_ms, u8 fc, u16 address, u16 value)
{
	u8	req[5] = { fc, (u8)(address >> 8), (u8)address, (u8)(value >> 8), (u8)value };
	u8	resp[260];

	request(dev, now_ms, req, 5, resp);
	return (resp[0] & 0x80) ? resp[1] : 0;
}

static u16
host_u16(device_t* dev, space_kind_t space, u32 address)
{
	u16 v = 0;

	ASSERT_SUCCESS(device_read(dev, space, address, 1, &v));
	return v;
}

#define COUNTER_ACTION \
	"(define-action counter\n" \
	"  (param step 1)\n" \
	"  (param limit 65535)\n" \
	"  (var n 0)\n" \
	"  (set! n (if (>= (+ n step) limit) 0 (+ n step)))\n" \
	"  (set! self n))\n"

TEST(ActionOnReadCounterRunsOncePerRead)
{
	device_t*	dev = load_device(SERVER COUNTER_ACTION
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (counter (step 5))))"
		" (holding 1 (static 0) (on-read (counter (limit 3)))))");
	u16		value = 0;

	ASSERT_SUCCESS(read_first(dev, 1000, 3, 0, 1, &value) == 0 && value == 5);
	ASSERT_SUCCESS(read_first(dev, 1001, 3, 0, 1, &value) == 0 && value == 10);
	ASSERT_SUCCESS(read_first(dev, 1002, 3, 0, 1, &value) == 0 && value == 15);
	/* Each use is its own instance with its own params: limit 3 wraps. */
	ASSERT_SUCCESS(read_first(dev, 1003, 3, 1, 1, &value) == 0 && value == 1);
	ASSERT_SUCCESS(read_first(dev, 1004, 3, 1, 1, &value) == 0 && value == 2);
	ASSERT_SUCCESS(read_first(dev, 1005, 3, 1, 1, &value) == 0 && value == 0);
	/* The point froze like after a client write, and no machine owns it. */
	ASSERT_EQ(dev->points[device_find_point(dev, SPACE_HOLDING, 0)].kind == POINT_STATIC);
}

TEST(ActionInlineBodyAndAddressOrder)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (var c 0) (set! c (+ c 1)) (set! self c)))"
		" (holding 1 (static 0) (on-read (set! self (* 2 (holding 0))))))");
	u8		req[5] = { 3, 0, 0, 0, 2 };
	u8		resp[260];

	/* Address order: holding 0 runs first, so holding 1 sees the new value. */
	request(dev, 1000, req, 5, resp);
	ASSERT_EQ(resp[1] == 4 && resp[3] == 1 && resp[5] == 2);
	request(dev, 1001, req, 5, resp);
	ASSERT_EQ(resp[3] == 2 && resp[5] == 4);
	/* Reading only the second point never runs the first action. */
	ASSERT_SUCCESS(read_first(dev, 1002, 3, 1, 1, NULL) == 0);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 0) == 2);
}

TEST(ActionOnReadOnDiscreteAndCoilFunctionCodes)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (discrete 0 (static #f) (on-read (set! self (not self))))"
		" (coil 0 (static #f) (on-read (set! self (not self))))"
		" (input 0 (static 0) (on-read (set! self (+ self 3)))))");
	u16		value = 0;

	ASSERT_SUCCESS(read_first(dev, 1000, 2, 0, 1, &value) == 0 && value == 1);
	ASSERT_SUCCESS(read_first(dev, 1001, 2, 0, 1, &value) == 0 && value == 0);
	ASSERT_SUCCESS(read_first(dev, 1002, 1, 0, 1, &value) == 0 && value == 1);
	ASSERT_SUCCESS(read_first(dev, 1003, 4, 0, 1, &value) == 0 && value == 3);
	ASSERT_SUCCESS(read_first(dev, 1004, 4, 0, 1, &value) == 0 && value == 6);
}

TEST(ActionOnReadRunsOnReadWriteMultipleRegistersReadHalf)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (set! self (+ self 1))))"
		" (holding 10 (static 0)))");
	/* FC 17: read 1 register at 0, write 1 register of 0x0007 at 10. */
	u8		req[12] = { 0x17, 0, 0, 0, 1, 0, 10, 0, 1, 2, 0, 7 };
	u8		resp[260];

	request(dev, 1000, req, 12, resp);
	ASSERT_EQ(resp[0] == 0x17 && resp[1] == 2 && resp[3] == 1);
	request(dev, 1001, req, 12, resp);
	ASSERT_EQ(resp[3] == 2);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 10) == 7);
}

TEST(ActionOnReadRunsInUserDefinedRegistersAction)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (set! self (+ self 1))))"
		" (function-code 65 (on \"\" (registers holding 0 1))))");
	u8		req[1] = { 0x41 };
	u8		resp[260];

	request(dev, 1000, req, 1, resp);
	ASSERT_EQ(resp[0] == 0x41 && resp[1] == 2 && resp[3] == 1);
	request(dev, 1001, req, 1, resp);
	ASSERT_EQ(resp[3] == 2);
}

TEST(ActionWideAndSecondRegisterOverlapRunOncePerRequest)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (type u32) (static 0) (on-read (set! self (+ self 1)))))");
	u8		req[5] = { 3, 0, 0, 0, 2 };
	u8		resp[260];

	request(dev, 1000, req, 5, resp);
	ASSERT_EQ(resp[1] == 4 && resp[5] == 1);
	request(dev, 1001, req, 5, resp);
	ASSERT_EQ(resp[5] == 2);
	/* A read of the second register alone still covers the point. */
	req[2] = 1;
	req[4] = 1;
	request(dev, 1002, req, 5, resp);
	ASSERT_EQ(resp[1] == 2 && resp[3] == 3);
}

static u8
veto(void* user, device_t* dev, const device_access_t* access)
{
	(void)user;
	(void)dev;
	(void)access;
	return 2;
}

TEST(ActionAccessHookVetoStopsActions)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (set! self (+ self 1)))))");
	u16		value = 0;

	dev->access_fn = veto;
	ASSERT_EQ(read_first(dev, 1000, 3, 0, 1, &value) == 2);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 0) == 0);
	dev->access_fn = NULL;
	ASSERT_SUCCESS(read_first(dev, 1001, 3, 0, 1, &value) == 0 && value == 1);
}

TEST(ActionHostAccessNeverRunsActions)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (set! self (+ self 1))))"
		" (holding 1 (static 0) (on-write (set! (holding 2) 9)))"
		" (holding 2 (static 0)))");
	u16		w = 5;
	val_t		v = { 0 };

	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 0) == 0);
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 0, &v));
	ASSERT_EQ(v.n == 0);
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 1, 1, &w));
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 2) == 0);
}

TEST(ActionOnWriteRunsAfterClientWrite)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 5))"
		" (coil 3 (static #f) (on-write (when self (set! (holding 0) 0)))))");

	ASSERT_EQ(write_single(dev, 1000, 5, 3, 0x0000) == 0);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 0) == 5);
	ASSERT_EQ(write_single(dev, 1001, 5, 3, 0xFF00) == 0);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 0) == 0);
}

TEST(ActionOnWriteDoesNotChainOrRecurse)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-write (set! self (+ self 1)) (set! (holding 1) 7)))"
		" (holding 1 (static 0) (on-write (set! (holding 2) 9)))"
		" (holding 2 (static 0)))");

	ASSERT_EQ(write_single(dev, 1000, 6, 0, 10) == 0);
	/* self was stored (10) and then incremented once by the action; holding 1's action did not run. */
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 0) == 11);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 1) == 7);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 2) == 0);
}

TEST(ActionFailureAnswersException04)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (set! self (/ 1 0))))"
		" (holding 1 (static 0) (on-write (set! self (/ 1 0))))"
		" (holding 2 (static 3)))");

	ASSERT_EQ(read_first(dev, 1000, 3, 0, 1, NULL) == 4);
	ASSERT_EQ(read_first(dev, 1001, 3, 2, 1, NULL) == 0);
	/* The write stays applied, the answer is exception 04. */
	ASSERT_EQ(write_single(dev, 1002, 6, 1, 8) == 4);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 1) == 8);
}

TEST(ActionRangeFailureAnswersException04)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (set! self (bit-and 0.5 1))))"
		" (holding 1 (static 0) (on-read (set! self (bit-and 6 3)))))");

	ASSERT_EQ(read_first(dev, 1000, 3, 0, 1, NULL) == 4);
	ASSERT_EQ(read_first(dev, 1001, 3, 1, 1, NULL) == 0);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 1) == 2);
}

TEST(ActionNowAndElapsedMs)
{
	device_t*	dev = load_device(SERVER
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (set! self (elapsed-ms))))"
		" (holding 1 (static 0) (on-read (set! self (- (now-ms) 1000)))))");
	u16		value = 0;

	ASSERT_SUCCESS(read_first(dev, 5000, 3, 0, 1, &value) == 0 && value == 0);
	ASSERT_SUCCESS(read_first(dev, 5250, 3, 0, 1, &value) == 0 && value == 250);
	ASSERT_SUCCESS(read_first(dev, 5300, 3, 1, 1, &value) == 0 && value == 4300);
}

TEST(ActionRestartResetsVarsKeepsParams)
{
	device_t*	dev = load_device(SERVER COUNTER_ACTION
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (counter (step 2)))))");
	u16		value = 0;
	val_t		step = { .type = VAL_NUM, .n = 7 };
	val_t		got = { 0 };

	ASSERT_SUCCESS(read_first(dev, 1000, 3, 0, 1, &value) == 0 && value == 2);
	ASSERT_SUCCESS(device_action_param_set(dev, SPACE_HOLDING, 0, ACTION_ON_READ, "step", &step));
	ASSERT_SUCCESS(read_first(dev, 1001, 3, 0, 1, &value) == 0 && value == 9);
	device_restart(dev, 2000);
	ASSERT_SUCCESS(device_action_param_get(dev, SPACE_HOLDING, 0, ACTION_ON_READ, "step", &got));
	ASSERT_EQ(got.n == 7);
	/* var n went back to 0 and the point to its configured value; the param stayed 7. */
	ASSERT_SUCCESS(read_first(dev, 2001, 3, 0, 1, &value) == 0 && value == 7);
}

TEST(ActionParamGetSetRules)
{
	device_t*	dev = load_device(SERVER
		"(define-action gate (param on #t) (param level 1.5) (set! self (if on level 0)))"
		"(device (unit-id 1) (holding 0 (static 0) (on-read (gate (level 4)))))");
	val_t		v = { 0 };
	val_t		flag = { .type = VAL_BOOL, .b = false };
	val_t		num = { .type = VAL_NUM, .n = 2 };
	const action_instance_t* inst = device_action_at(dev, 0);

	ASSERT_SUCCESS(inst != NULL && device_action_at(dev, 1) == NULL);
	ASSERT_EQ(inst->param_count == 2 && inst->var_count == 2);
	ASSERT_EQ(strcmp(inst->name, "gate") == 0 && strcmp(inst->defs[0].name, "on") == 0);
	ASSERT_SUCCESS(device_action_param_get(dev, SPACE_HOLDING, 0, ACTION_ON_READ, "level", &v));
	ASSERT_EQ(v.type == VAL_NUM && v.n == 4);
	ASSERT_SUCCESS(device_action_param_get(dev, SPACE_HOLDING, 0, ACTION_ON_READ, "on", &v));
	ASSERT_EQ(v.type == VAL_BOOL && v.b);
	ASSERT_FAILURE(device_action_param_get(dev, SPACE_HOLDING, 0, ACTION_ON_READ, "nope", &v));
	ASSERT_FAILURE(device_action_param_get(dev, SPACE_HOLDING, 0, ACTION_ON_WRITE, "on", &v));
	ASSERT_FAILURE(device_action_param_get(dev, SPACE_HOLDING, 1, ACTION_ON_READ, "on", &v));
	ASSERT_FAILURE(device_action_param_set(dev, SPACE_HOLDING, 0, ACTION_ON_READ, "on", &num));
	ASSERT_SUCCESS(device_action_param_set(dev, SPACE_HOLDING, 0, ACTION_ON_READ, "on", &flag));
	ASSERT_SUCCESS(read_first(dev, 1000, 3, 0, 1, NULL) == 0);
	ASSERT_EQ(host_u16(dev, SPACE_HOLDING, 0) == 0);
}

TEST(ActionConfigErrors)
{
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (nothing))))",
		     "unknown action nothing");
	EXPECT_ERROR(SERVER COUNTER_ACTION "(device (unit-id 1) (holding 0 (on-read (counter (nope 1)))))",
		     "unknown param 'nope' for action 'counter'");
	EXPECT_ERROR(SERVER COUNTER_ACTION "(device (unit-id 1) (holding 0 (on-read (counter (step #t)))))",
		     "param 'step' takes a number");
	EXPECT_ERROR(SERVER COUNTER_ACTION "(device (unit-id 1) (holding 0 (on-read (counter (step 1) (step 2)))))",
		     "given more than once");
	EXPECT_ERROR(SERVER "(define-action a (param p 1) (set! p 2) (set! self 1))"
		     "(device (unit-id 1) (holding 0 (on-read (a))))",
		     "a param is read-only");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (param p 1) (set! self p))))",
		     "param is only valid in define-action");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (count 2) (on-read (set! self 1))))",
		     "on-read needs a single point");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (type string) (length 2) (static \"ab\") (on-read (set! self 1))))",
		     "not valid on a string point");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (input 0 (on-write (set! self 1))))",
		     "on-write is only valid on coil and holding points");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (access read-only) (on-write (set! self 1))))",
		     "on-write is not valid on a read-only point");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read)))", "on-read needs a body");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (set! self 1)) (on-read (set! self 2))))",
		     "more than once");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (set! (holding 9) 1))))",
		     "set! target must name the first address of a configured point");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (set! self (holding 9)))))",
		     "point read must name the first address of a configured point");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (set! self (holding 1)))) (holding 1 (type string) (length 1)))",
		     "a string point has no number to read or write");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (set! self #t))))",
		     "assignment value has the wrong type");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (static 0)"
		     " (on-read (set! self 1)))"
		     " (machine m (initial s) (state s (on-tick (set! (holding 0) (+ self 1))))))",
		     "self is only valid in point actions");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0) (holding 1 (on-read (set! (holding 0) 1)))"
		     " (machine m (initial s) (state s (on-tick (set! (holding 0) 2)))))",
		     "is driven by a machine");
}

TEST(ActionDefineErrors)
{
	EXPECT_ERROR(SERVER "(define-action Bad (set! self 1))", "bad action name");
	EXPECT_ERROR(SERVER "(define-action min (set! self 1))", "bad action name");
	EXPECT_ERROR(SERVER "(define-action self (set! self 1))", "bad action name");
	EXPECT_ERROR(SERVER "(define-action when (set! self 1))", "bad action name");
	EXPECT_ERROR(SERVER "(define-action a (set! self 1)) (define-action a (set! self 2))",
		     "defined more than once");
	EXPECT_ERROR(SERVER "(define-action a (param p 1))", "at least one statement");
	EXPECT_ERROR(SERVER "(define-action a (var v 0) (param p 1) (set! v 1))", "param must precede var");
	EXPECT_ERROR(SERVER "(define-action a (set! self 1) (var v 0))", "param and var must come first");
	EXPECT_ERROR(SERVER "(define-action a (param self 1) (set! self 1))", "self is a reserved name");
	EXPECT_ERROR(SERVER "(define-action a (param p 1) (param p 2) (set! self 1))", "duplicate variable name");
	EXPECT_ERROR(SERVER "(define-action a (param p x) (set! self 1))", "param literal must be");
	EXPECT_ERROR(SERVER "(device (unit-id 1) (holding 0 (on-read (late))))"
		     "(define-action late (set! self 1))",
		     "unknown action late");
}

TEST(ActionLimits)
{
	char	text[200000];
	u32	n = (u32)snprintf(text, sizeof(text), SERVER);
	config_t cfg = { 0 };
	char	err[1024] = "";

	for (u32 i = 0; i < 65; i++)
		n += (u32)snprintf(text + n, sizeof(text) - n, "(define-action a%u (set! self 1))\n", i);
	EXPECT_ERROR(text, "too many define-action forms (max 64)");

	n = (u32)snprintf(text, sizeof(text), SERVER "(device (unit-id 1)");
	for (u32 i = 0; i < 257; i++)
		n += (u32)snprintf(text + n, sizeof(text) - n, " (coil %u (on-read (set! self #t)))", i);
	snprintf(text + n, sizeof(text) - n, ")");
	EXPECT_ERROR(text, "too many point actions on one device (max 256)");

	n = (u32)snprintf(text, sizeof(text), SERVER "(device (unit-id 1)");
	for (u32 i = 0; i < 256; i++)
		n += (u32)snprintf(text + n, sizeof(text) - n, " (coil %u (on-read (set! self #t)))", i);
	snprintf(text + n, sizeof(text) - n, ")");
	ASSERT_SUCCESS(load_text(text, &cfg, err, sizeof(err)));
	ASSERT_EQ(cfg.devices[0]->actions->count == 256);
}

TEST(ActionDefinitionsAreSharedByDevicesAndSeparateInstances)
{
	static config_t c;
	char		e[1024] = "";
	u16		value = 0;

	memset(&c, 0, sizeof(c));
	ASSERT_SUCCESS(load_text(SERVER COUNTER_ACTION
		"(device (unit-id 1) (holding 0 (static 0) (on-read (counter))))"
		"(device (unit-id 2) (holding 0 (static 0) (on-read (counter))))", &c, e, sizeof(e)));
	ASSERT_SUCCESS(read_first(c.devices[0], 1000, 3, 0, 1, &value) == 0 && value == 1);
	ASSERT_SUCCESS(read_first(c.devices[0], 1001, 3, 0, 1, &value) == 0 && value == 2);
	ASSERT_SUCCESS(read_first(c.devices[1], 1002, 3, 0, 1, &value) == 0 && value == 1);
}

TEST(ActionPublicListingShowsParams)
{
	device_t*	dev = load_device(SERVER COUNTER_ACTION
		"(device (unit-id 1)"
		" (holding 0 (static 0) (on-read (counter (step 5))))"
		" (coil 1 (static #f) (on-write (set! self #f))))");
	mogbus_action_t	a;

	ASSERT_SUCCESS(mogbus_device_action_at((const mogbus_device_t*)dev, 0, &a));
	ASSERT_EQ(a.space == MOGBUS_COIL && a.address == 1 && a.on_write && a.name[0] == '\0');
	ASSERT_SUCCESS(mogbus_device_action_at((const mogbus_device_t*)dev, 1, &a));
	ASSERT_EQ(a.space == MOGBUS_HOLDING && !a.on_write && strcmp(a.name, "counter") == 0);
	ASSERT_EQ(a.param_count == 2 && strcmp(a.params[0].name, "step") == 0 &&
		  !a.params[0].is_bool && a.params[0].number == 5);
	ASSERT_FAILURE(mogbus_device_action_at((const mogbus_device_t*)dev, 2, &a));
}
