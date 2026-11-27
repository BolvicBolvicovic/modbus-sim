/* The Modbus client (mogbus_client_*) against a real library server. The server runs on its own
 * thread and only ever talks to the client over a socket (or a PTY): the client never steps it. */
#define _GNU_SOURCE
#include "vtest.h"
#include "tests/tests_utils/test_case.h"
#include "mogbus.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum
{
	CUSTOM_FC       = 0x41,	/* reverses its data */
	SILENT_FC       = 0x42,	/* counts the request and never answers */
	REFUSING_FC     = 0x43,	/* answers exception 0x05 */
	HOLDING_COUNT   = 10,
	COIL_COUNT      = 16,
	CALL_TIMEOUT_MS  = 200,
};

typedef struct fx_t fx_t;

struct fx_t
{
	mogbus_t*		ctx;
	mogbus_server_t*	server;
	mogbus_mode_t		mode;
	u16			port;
	pthread_t		thread;
	atomic_bool		stop;
	atomic_int		silent_hits;
};

static int
reverse_fn(void* user, mogbus_device_t* dev, uint8_t unit, uint8_t fc, const uint8_t* data, size_t len,
	   uint8_t* reply, size_t cap, size_t* reply_len)
{
	(void)user; (void)dev; (void)unit; (void)fc;
	if (len > cap)
		return 3;
	for (size_t i = 0; i < len; i++)
		reply[i] = data[len - 1 - i];
	*reply_len = len;
	return 0;
}

static int
silent_fn(void* user, mogbus_device_t* dev, uint8_t unit, uint8_t fc, const uint8_t* data, size_t len,
	  uint8_t* reply, size_t cap, size_t* reply_len)
{
	(void)dev; (void)unit; (void)fc; (void)data; (void)len; (void)reply; (void)cap; (void)reply_len;
	atomic_fetch_add(&((fx_t*)user)->silent_hits, 1);
	return MOGBUS_FUNCTION_NO_RESPONSE;
}

static int
refusing_fn(void* user, mogbus_device_t* dev, uint8_t unit, uint8_t fc, const uint8_t* data, size_t len,
	    uint8_t* reply, size_t cap, size_t* reply_len)
{
	(void)user; (void)dev; (void)unit; (void)fc; (void)data; (void)len; (void)reply; (void)cap; (void)reply_len;
	return 5;
}

static void*
server_thread(void* arg)
{
	fx_t* fx = arg;

	while (!atomic_load(&fx->stop))
		(void)mogbus_server_step(fx->server, 5);
	return NULL;
}

static bool
server_up(fx_t* fx)
{
	mogbus_server_options_t o = mogbus_server_options_defaults();

	o.mode = fx->mode;
	o.port = fx->port;
	fx->server = mogbus_server_create(fx->ctx, &o);
	if (!fx->server)
		return false;
	atomic_store(&fx->stop, false);
	return pthread_create(&fx->thread, NULL, server_thread, fx) == 0;
}

static void
server_down(fx_t* fx)
{
	if (!fx->server)
		return;
	atomic_store(&fx->stop, true);
	pthread_join(fx->thread, NULL);
	mogbus_server_close(fx->server);
	fx->server = NULL;
}

/* One device (unit 1) with holding 0-9 = 100+i, input 0-3 = 200+i, coils 0-15 (odd on) and
 * discrete inputs 0-7 (even on), plus the three function codes above. */
static bool
fx_start(fx_t* fx, mogbus_mode_t mode)
{
	mogbus_device_t* dev;

	memset(fx, 0, sizeof(*fx));
	fx->mode = mode;
	fx->port = ts_free_port();
	fx->ctx  = mogbus_create();
	if (!fx->ctx || fx->port == 0)
		return false;
	dev = mogbus_add_device(fx->ctx, 1, "client-test");
	if (!dev)
		return false;
	for (uint32_t i = 0; i < 40; i++)
	{
		mogbus_point_t p = { .space = MOGBUS_HOLDING, .address = i, .type = MOGBUS_TYPE_U16, .value = 100 + i };

		if (mogbus_device_add_point(dev, &p) != MOGBUS_OK)
			return false;
	}
	for (uint32_t i = 0; i < 4; i++)
	{
		mogbus_point_t p = { .space = MOGBUS_INPUT, .address = i, .type = MOGBUS_TYPE_U16, .value = 200 + i };

		if (mogbus_device_add_point(dev, &p) != MOGBUS_OK)
			return false;
	}
	for (uint32_t i = 0; i < COIL_COUNT; i++)
	{
		mogbus_point_t p = { .space = MOGBUS_COIL, .address = i, .type = MOGBUS_TYPE_BOOL, .value = i & 1 };

		if (mogbus_device_add_point(dev, &p) != MOGBUS_OK)
			return false;
	}
	for (uint32_t i = 0; i < 8; i++)
	{
		mogbus_point_t p = { .space = MOGBUS_DISCRETE, .address = i, .type = MOGBUS_TYPE_BOOL, .value = !(i & 1) };

		if (mogbus_device_add_point(dev, &p) != MOGBUS_OK)
			return false;
	}
	return mogbus_device_add_function_code(dev, CUSTOM_FC, reverse_fn, fx) &&
	       mogbus_device_add_function_code(dev, SILENT_FC, silent_fn, fx) &&
	       mogbus_device_add_function_code(dev, REFUSING_FC, refusing_fn, fx) &&
	       server_up(fx);
}

static void
fx_stop(fx_t* fx)
{
	server_down(fx);
	mogbus_destroy(fx->ctx);
	fx->ctx = NULL;
}

static mogbus_client_t*
client_for(const fx_t* fx, uint32_t retries)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();

	o.mode       = fx->mode;
	o.port       = fx->port;
	o.timeout_ms = CALL_TIMEOUT_MS;
	o.retries    = retries;
	return mogbus_client_open(&o);
}

/* Every typed call, in the framing of fx->mode. */
static void
exercise_all_calls(mogbus_client_t* c)
{
	uint8_t		bits[COIL_COUNT];
	uint16_t	regs[8];
	uint16_t	back[8];
	uint8_t		ids[252];
	size_t		ids_len = 0;
	size_t		n = 0;
	uint8_t		reply[8];

	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 5, regs) == MOGBUS_OK);
	for (int i = 0; i < 5; i++)
		ASSERT_EQ(regs[i] == 100 + i);
	ASSERT_EQ(mogbus_client_read_input_registers(c, 1, 1, 3, regs) == MOGBUS_OK);
	ASSERT_EQ(regs[0] == 201 && regs[1] == 202 && regs[2] == 203);
	ASSERT_EQ(mogbus_client_read_coils(c, 1, 0, COIL_COUNT, bits) == MOGBUS_OK);
	for (int i = 0; i < COIL_COUNT; i++)
		ASSERT_EQ(bits[i] == (i & 1));
	ASSERT_EQ(mogbus_client_read_discrete_inputs(c, 1, 0, 8, bits) == MOGBUS_OK);
	for (int i = 0; i < 8; i++)
		ASSERT_EQ(bits[i] == !(i & 1));

	ASSERT_EQ(mogbus_client_write_coil(c, 1, 0, true) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_coils(c, 1, 0, 2, bits) == MOGBUS_OK);
	ASSERT_EQ(bits[0] == 1 && bits[1] == 1);
	ASSERT_EQ(mogbus_client_write_register(c, 1, 0, 0xBEEF) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, regs) == MOGBUS_OK);
	ASSERT_EQ(regs[0] == 0xBEEF);

	{
		uint8_t on[11] = { 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0 };

		ASSERT_EQ(mogbus_client_write_coils(c, 1, 2, 11, on) == MOGBUS_OK);
		ASSERT_EQ(mogbus_client_read_coils(c, 1, 2, 11, bits) == MOGBUS_OK);
		ASSERT_EQ(memcmp(bits, on, 11) == 0);
	}
	{
		uint16_t w[3] = { 11, 22, 33 };

		ASSERT_EQ(mogbus_client_write_registers(c, 1, 10, 3, w) == MOGBUS_OK);
		ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 10, 3, regs) == MOGBUS_OK);
		ASSERT_EQ(memcmp(regs, w, sizeof(w)) == 0);
	}
	ASSERT_EQ(mogbus_client_write_register(c, 1, 20, 0x00F0) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_mask_write_register(c, 1, 20, 0x00FF, 0x0F00) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 20, 1, regs) == MOGBUS_OK);
	ASSERT_EQ(regs[0] == 0x0FF0);
	{
		uint16_t w[2] = { 7, 8 };

		ASSERT_EQ(mogbus_client_read_write_registers(c, 1, 30, 2, back, 30, 2, w) == MOGBUS_OK);
		ASSERT_EQ(back[0] == 7 && back[1] == 8);
	}

	ASSERT_EQ(mogbus_client_read_device_id(c, 1, ids, sizeof(ids), &ids_len) == MOGBUS_OK);
	ASSERT_EQ(ids_len >= 3 && ids[0] == 0);

	{
		uint8_t data[3] = { 1, 2, 3 };

		ASSERT_EQ(mogbus_client_request(c, 1, CUSTOM_FC, data, 3, reply, sizeof(reply), &n) == MOGBUS_OK);
		ASSERT_EQ(n == 3 && reply[0] == 3 && reply[1] == 2 && reply[2] == 1);
	}
}

TEST(ClientOptionsAndOpenErrors)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();

	ASSERT_EQ(o.mode == MOGBUS_MODE_TCP && o.port == 1502 && strcmp(o.host, "127.0.0.1") == 0);
	ASSERT_EQ(o.timeout_ms == 1000 && o.retries == 0 && o.data_bits == 8 && o.stop_bits == 1);

	ASSERT_EQ(mogbus_client_open(NULL) == NULL);
	ASSERT_EQ(strlen(mogbus_last_error(NULL)) > 0);

	o.host = "not-an-ip";
	ASSERT_EQ(mogbus_client_open(&o) == NULL);
	ASSERT_EQ(strstr(mogbus_last_error(NULL), "not-an-ip") != NULL);
	o.host = "127.0.0.1";

	o.port = 0;
	ASSERT_EQ(mogbus_client_open(&o) == NULL);
	o.port = 1502;

	o.mode = MOGBUS_MODE_RTU_PTY;
	ASSERT_EQ(mogbus_client_open(&o) == NULL);
	ASSERT_EQ(strstr(mogbus_last_error(NULL), "not a client mode") != NULL);
	o.mode = MOGBUS_MODE_TCP;

	o.tls_enabled = true;
	ASSERT_EQ(mogbus_client_open(&o) == NULL);
	ASSERT_EQ(strstr(mogbus_last_error(NULL), "TLS") != NULL);
	o.tls_enabled = false;

	o.mode = MOGBUS_MODE_RTU_DEVICE;
	o.path = NULL;
	ASSERT_EQ(mogbus_client_open(&o) == NULL);
	o.mode = MOGBUS_MODE_TCP;

	{
		mogbus_client_t* c = mogbus_client_open(&o);

		ASSERT_EQ(c != NULL);
		ASSERT_EQ(strlen(mogbus_last_error(NULL)) == 0);	/* a success clears it */
		ASSERT_EQ(strcmp(mogbus_client_last_error(c), "") == 0);
		ASSERT_EQ(mogbus_client_last_exception(c) == 0);
		mogbus_client_close(c);
	}
	mogbus_client_close(NULL);
}

TEST(ClientArgumentChecks)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();
	mogbus_client_t*	c = mogbus_client_open(&o);
	uint16_t		regs[200];
	uint8_t			bits[2100];
	size_t			n = 7;

	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 0, regs) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 126, regs) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 0, 0, 1, regs) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(strlen(mogbus_client_last_error(c)) > 0);
	ASSERT_EQ(mogbus_client_read_coils(c, 1, 0, 2001, bits) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_write_registers(c, 1, 0, 124, regs) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_write_coils(c, 1, 0, 1969, bits) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_request(c, 1, 0, NULL, 0, NULL, 0, &n) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(n == 0);
	ASSERT_EQ(mogbus_client_request(c, 1, 3, NULL, 0, NULL, 0, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_read_coils(NULL, 1, 0, 1, bits) == MOGBUS_ERR_INVALID);
	/* nothing was sent, so the unreachable default endpoint was never tried */
	mogbus_client_close(c);
}

TEST(ClientTcpEveryCall)
{
	fx_t			fx;
	mogbus_client_t*	c;

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP));
	c = client_for(&fx, 0);
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_connect(c) == MOGBUS_OK);
	exercise_all_calls(c);
	mogbus_client_close(c);
	fx_stop(&fx);
}

TEST(ClientFramingOverNetworkModes)
{
	static const mogbus_mode_t modes[] = {
		MOGBUS_MODE_UDP, MOGBUS_MODE_RTU_TCP, MOGBUS_MODE_RTU_UDP,
		MOGBUS_MODE_ASCII_TCP, MOGBUS_MODE_ASCII_UDP,
	};
	static const char* names[] = { "udp", "rtu-tcp", "rtu-udp", "ascii-tcp", "ascii-udp" };

	for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
	{
		fx_t			fx;
		mogbus_client_t*	c;

		test_case = names[m];
		ASSERT_EQ(fx_start(&fx, modes[m]));
		c = client_for(&fx, 0);
		ASSERT_EQ(c != NULL);
		exercise_all_calls(c);
		mogbus_client_close(c);
		fx_stop(&fx);
	}
	test_case = NULL;
}

TEST(ClientExceptionReply)
{
	fx_t			fx;
	mogbus_client_t*	c;
	uint16_t		regs[4];
	size_t			n = 9;
	uint8_t			reply[4];

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP));
	c = client_for(&fx, 0);
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 5000, 1, regs) == MOGBUS_ERR_EXCEPTION);
	ASSERT_EQ(mogbus_client_last_exception(c) == 2);
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "exception 0x02") != NULL);
	/* an exception is an answer: the connection stays usable */
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 1, 1, regs) == MOGBUS_OK && regs[0] == 101);
	ASSERT_EQ(mogbus_client_write_register(c, 1, 5000, 1) == MOGBUS_ERR_EXCEPTION);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 9, 0, 1, regs) == MOGBUS_ERR_EXCEPTION);
	ASSERT_EQ(mogbus_client_last_exception(c) == 11);	/* unknown unit: gateway target */
	ASSERT_EQ(mogbus_client_request(c, 1, 0x7F, NULL, 0, reply, sizeof(reply), &n) == MOGBUS_ERR_EXCEPTION);
	ASSERT_EQ(mogbus_client_last_exception(c) == 1 && n == 0);
	ASSERT_EQ(mogbus_client_request(c, 1, REFUSING_FC, NULL, 0, reply, sizeof(reply), &n) == MOGBUS_ERR_EXCEPTION);
	ASSERT_EQ(mogbus_client_last_exception(c) == 5);
	mogbus_client_close(c);
	fx_stop(&fx);
}

TEST(ClientTimeoutRetriesAndRecovery)
{
	fx_t			fx;
	mogbus_client_t*	c;
	uint16_t		regs[1];
	size_t			n = 0;
	u64			t0;

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP));
	c = client_for(&fx, 2);
	ASSERT_EQ(c != NULL);
	t0 = os_now_ms();
	ASSERT_EQ(mogbus_client_request(c, 1, SILENT_FC, NULL, 0, NULL, 0, &n) == MOGBUS_ERR_TIMEOUT);
	ASSERT_EQ(os_now_ms() - t0 >= 3 * CALL_TIMEOUT_MS - 20);
	ASSERT_EQ(atomic_load(&fx.silent_hits) == 3);	/* the call and its two retries */
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "no reply") != NULL);
	/* the connection was dropped with the timeout, so the next call reconnects and works */
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 2, 1, regs) == MOGBUS_OK && regs[0] == 102);
	mogbus_client_close(c);

	/* retries are per call: 1 + 5 attempts */
	c = client_for(&fx, 5);
	ASSERT_EQ(mogbus_client_request(c, 1, SILENT_FC, NULL, 0, NULL, 0, &n) == MOGBUS_ERR_TIMEOUT);
	ASSERT_EQ(atomic_load(&fx.silent_hits) == 3 + 6);
	mogbus_client_close(c);
	fx_stop(&fx);
}

TEST(ClientUdpTimeoutThenWorks)
{
	fx_t			fx;
	mogbus_client_t*	c;
	uint16_t		regs[1];
	size_t			n = 0;

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_UDP));
	c = client_for(&fx, 0);
	ASSERT_EQ(mogbus_client_request(c, 1, SILENT_FC, NULL, 0, NULL, 0, &n) == MOGBUS_ERR_TIMEOUT);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 3, 1, regs) == MOGBUS_OK && regs[0] == 103);
	mogbus_client_close(c);
	fx_stop(&fx);
}

TEST(ClientReconnectsAfterServerRestart)
{
	fx_t			fx;
	mogbus_client_t*	c;
	uint16_t		regs[1];

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP));
	c = client_for(&fx, 0);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, regs) == MOGBUS_OK);

	server_down(&fx);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, regs) == MOGBUS_ERR_IO);
	ASSERT_EQ(strlen(mogbus_client_last_error(c)) > 0);
	/* nobody listens: connecting fails too, and says so */
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, regs) == MOGBUS_ERR_IO);
	ASSERT_EQ(mogbus_client_connect(c) == MOGBUS_ERR_IO);
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "connect") != NULL);

	ASSERT_EQ(server_up(&fx));
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 4, 1, regs) == MOGBUS_OK && regs[0] == 104);
	mogbus_client_close(c);
	fx_stop(&fx);
}

TEST(ClientRawRequestAndBroadcast)
{
	fx_t			fx;
	mogbus_client_t*	c;
	uint8_t			reply[300];
	size_t			n = 99;
	uint8_t			data[252];
	uint16_t		regs[1];

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_RTU_TCP));
	c = client_for(&fx, 0);
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)i;
	/* the largest request the protocol allows, the largest reply, a reply buffer that is too small */
	ASSERT_EQ(mogbus_client_request(c, 1, CUSTOM_FC, data, sizeof(data), reply, sizeof(reply), &n) == MOGBUS_OK);
	ASSERT_EQ(n == sizeof(data) && reply[0] == 251 && reply[251] == 0);
	ASSERT_EQ(mogbus_client_request(c, 1, CUSTOM_FC, data, 10, reply, 4, &n) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(n == 0);
	ASSERT_EQ(mogbus_client_request(c, 1, CUSTOM_FC, data, 253, reply, sizeof(reply), &n) == MOGBUS_ERR_INVALID);
	/* unit 0 broadcasts on RTU: sent, nothing awaited, nothing executed (broadcast-execute is off) */
	ASSERT_EQ(mogbus_client_write_register(c, 0, 0, 1) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, regs) == MOGBUS_OK && regs[0] == 100);
	mogbus_client_close(c);
	fx_stop(&fx);
}

/* A client over a serial line: the server's PTY slave is a device path. */
static void
serial_round_trip(mogbus_mode_t server_mode, mogbus_mode_t client_mode)
{
	mogbus_t*		ctx = mogbus_create();
	mogbus_device_t*	dev = mogbus_add_device(ctx, 1, "serial");
	mogbus_point_t		p = { .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_U16, .value = 4242 };
	mogbus_server_options_t so = mogbus_server_options_defaults();
	mogbus_client_options_t co = mogbus_client_options_defaults();
	fx_t			fx = { .ctx = ctx };
	mogbus_client_t*	c;
	uint16_t		regs[2];
	uint16_t		w = 77;

	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	p.address = 1;
	ASSERT_EQ(mogbus_device_add_point(dev, &p) == MOGBUS_OK);
	so.mode    = server_mode;
	fx.server  = mogbus_server_create(ctx, &so);
	ASSERT_EQ(fx.server != NULL);
	atomic_store(&fx.stop, false);
	ASSERT_EQ(pthread_create(&fx.thread, NULL, server_thread, &fx) == 0);

	co.mode       = client_mode;
	co.path       = mogbus_server_pty_name(fx.server);
	co.timeout_ms = 2000;
	c = mogbus_client_open(&co);
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 2, regs) == MOGBUS_OK);
	ASSERT_EQ(regs[0] == 4242 && regs[1] == 4242);
	ASSERT_EQ(mogbus_client_write_registers(c, 1, 1, 1, &w) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 2, regs) == MOGBUS_OK && regs[1] == 77);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 900, 1, regs) == MOGBUS_ERR_EXCEPTION);
	ASSERT_EQ(mogbus_client_last_exception(c) == 2);
	mogbus_client_close(c);
	fx_stop(&fx);
}

TEST(ClientSerialRtuOverPty)
{
	serial_round_trip(MOGBUS_MODE_RTU_PTY, MOGBUS_MODE_RTU_DEVICE);
}

TEST(ClientSerialAsciiOverPty)
{
	serial_round_trip(MOGBUS_MODE_ASCII_PTY, MOGBUS_MODE_ASCII_DEVICE);
}

TEST(ClientSerialDeviceMissing)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();
	mogbus_client_t*	c;

	o.mode = MOGBUS_MODE_RTU_DEVICE;
	o.path = "/nonexistent/ttyX";
	c = mogbus_client_open(&o);
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_connect(c) == MOGBUS_ERR_IO);
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "/nonexistent/ttyX") != NULL);
	mogbus_client_close(c);
}

TEST(ClientIsIndependentOfAnyContext)
{
	/* Two clients of one server, neither touching the server's context. */
	fx_t			fx;
	mogbus_client_t*	a;
	mogbus_client_t*	b;
	uint16_t		x[1];
	uint16_t		y[1];

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP));
	a = client_for(&fx, 0);
	b = client_for(&fx, 0);
	ASSERT_EQ(mogbus_client_read_holding_registers(a, 1, 6, 1, x) == MOGBUS_OK && x[0] == 106);
	ASSERT_EQ(mogbus_client_read_holding_registers(b, 1, 7, 1, y) == MOGBUS_OK && y[0] == 107);
	ASSERT_EQ(mogbus_client_read_holding_registers(a, 1, 6, 1, x) == MOGBUS_OK);
	mogbus_client_close(a);
	mogbus_client_close(b);
	fx_stop(&fx);
}
