/* Thread safety of the library: one context used by a serving thread, host threads and a client
 * at once, callbacks that call back into the context, and a stop from another thread. */
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
	HAMMER_ROUNDS = 20000,
	CLIENT_READS  = 300,
	REPLY_WAIT_MS = 3000,
	STOP_BOUND_MS = 1500,	/* the documented bound is one second */
	CUSTOM_FC     = 0x41,
};

typedef struct fx_t fx_t;
typedef struct hammer_t hammer_t;

struct fx_t
{
	mogbus_t*		ctx;
	mogbus_device_t*	dev;
	mogbus_server_t*	server;
	u16			port;
	pthread_t		thread;
	atomic_int		hook_calls;
	atomic_int		fc_calls;
};

struct hammer_t
{
	fx_t*		fx;
	uint32_t	address;
	bool		ok;
	pthread_t	thread;
};

/* The access hook runs on the serving thread with the context locked and calls the context again. */
static uint8_t
reentrant_access(void* user, mogbus_device_t* dev, const mogbus_access_t* access)
{
	fx_t*	fx    = user;
	double	value = 0;

	(void)access;
	atomic_fetch_add(&fx->hook_calls, 1);
	if (mogbus_device_get_value(dev, MOGBUS_HOLDING, 2, &value) != MOGBUS_OK ||
	    mogbus_device_set_value(dev, MOGBUS_HOLDING, 2, value + 1) != MOGBUS_OK)
		return 4;
	return 0;
}

static int
reentrant_fc(void* user, mogbus_device_t* dev, uint8_t unit, uint8_t fc, const uint8_t* data, size_t len,
	     uint8_t* reply, size_t cap, size_t* reply_len)
{
	fx_t* fx = user;

	(void)unit; (void)fc; (void)data; (void)len; (void)cap;
	atomic_fetch_add(&fx->fc_calls, 1);
	if (mogbus_device_set_value(dev, MOGBUS_HOLDING, 3, 7) != MOGBUS_OK)
		return 4;
	reply[0]   = 0x2a;
	*reply_len = 1;
	return 0;
}

static void*
serve_thread(void* arg)
{
	fx_t* fx = arg;

	mogbus_server_run(fx->server);
	return NULL;
}

static void*
hammer_thread(void* arg)
{
	hammer_t* h = arg;

	h->ok = true;
	for (uint32_t i = 0; i < HAMMER_ROUNDS; i++)
	{
		double value = 0;

		if (mogbus_device_set_value(h->fx->dev, MOGBUS_HOLDING, h->address, i % 60000) != MOGBUS_OK ||
		    mogbus_device_get_value(h->fx->dev, MOGBUS_HOLDING, h->address, &value) != MOGBUS_OK ||
		    value != (double)(i % 60000))
			h->ok = false;
	}
	return NULL;
}

static bool
fx_start(fx_t* fx)
{
	mogbus_server_options_t o = mogbus_server_options_defaults();

	memset(fx, 0, sizeof(*fx));
	fx->port = ts_free_port();
	if (fx->port == 0 ||
	    mogbus_create(&fx->ctx) != MOGBUS_OK ||
	    mogbus_add_device(fx->ctx, 1, "threads", &fx->dev) != MOGBUS_OK)
		return false;
	for (uint32_t i = 0; i < 4; i++)
	{
		mogbus_point_t p = { .space = MOGBUS_HOLDING, .address = i, .type = MOGBUS_TYPE_U16 };

		if (mogbus_device_add_point(fx->dev, &p) != MOGBUS_OK)
			return false;
	}
	o.port = fx->port;
	if (mogbus_server_create(fx->ctx, &o, &fx->server) != MOGBUS_OK)
		return false;
	return pthread_create(&fx->thread, NULL, serve_thread, fx) == 0;
}

/* Stops from this thread (not a signal); returns how long mogbus_server_run took to return. */
static u64
fx_stop(fx_t* fx)
{
	u64 start = os_now_ms();

	mogbus_server_stop(fx->server);
	pthread_join(fx->thread, NULL);
	start = os_now_ms() - start;
	mogbus_destroy(fx->ctx);
	fx->ctx = NULL;
	return start;
}

static mogbus_client_t*
client_for(const fx_t* fx)
{
	mogbus_client_options_t o      = mogbus_client_options_defaults();
	mogbus_client_t*	client = NULL;

	o.port       = fx->port;
	o.timeout_ms = REPLY_WAIT_MS;
	mogbus_client_open(&o, &client);
	return client;
}

TEST(HostThreadsAndClientShareARunningServer)
{
	fx_t			fx;
	hammer_t		a = { .address = 0 };
	hammer_t		b = { .address = 1 };
	mogbus_client_t*	client;
	bool			reads_ok = true;

	ASSERT_EQ(fx_start(&fx));
	a.fx = b.fx = &fx;
	client = client_for(&fx);
	ASSERT_EQ(client != NULL);
	ASSERT_EQ(pthread_create(&a.thread, NULL, hammer_thread, &a) == 0);
	ASSERT_EQ(pthread_create(&b.thread, NULL, hammer_thread, &b) == 0);
	for (u32 i = 0; i < CLIENT_READS; i++)
	{
		uint16_t regs[2] = { 0, 0 };

		if (mogbus_client_read_holding_registers(client, 1, 0, 2, regs) != MOGBUS_OK ||
		    regs[0] >= 60000 ||
		    regs[1] >= 60000)
			reads_ok = false;
	}
	pthread_join(a.thread, NULL);
	pthread_join(b.thread, NULL);
	mogbus_client_close(client);
	ASSERT_EQ(a.ok && b.ok);
	ASSERT_EQ(reads_ok);
	ASSERT_EQ(fx_stop(&fx) < STOP_BOUND_MS);
}

TEST(CallbacksMayCallBackIntoTheContext)
{
	fx_t			fx;
	mogbus_client_t*	client;
	uint16_t		regs[1] = { 0 };
	uint8_t			reply[8];
	size_t			reply_len = 0;
	double			value = 0;

	ASSERT_EQ(fx_start(&fx));
	ASSERT_EQ(mogbus_device_set_access_hook(fx.dev, reentrant_access, &fx) == MOGBUS_OK);
	ASSERT_EQ(mogbus_device_add_function_code(fx.dev, CUSTOM_FC, reentrant_fc, &fx) == MOGBUS_OK);
	client = client_for(&fx);
	ASSERT_EQ(client != NULL);
	ASSERT_EQ(mogbus_client_read_holding_registers(client, 1, 0, 1, regs) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(client, 1, 0, 1, regs) == MOGBUS_OK);
	ASSERT_EQ(atomic_load(&fx.hook_calls) == 2);
	ASSERT_EQ(mogbus_device_get_value(fx.dev, MOGBUS_HOLDING, 2, &value) == MOGBUS_OK && value == 2);
	ASSERT_EQ(mogbus_client_request(client, 1, CUSTOM_FC, NULL, 0, reply, sizeof(reply), &reply_len) == MOGBUS_OK);
	ASSERT_EQ(reply_len == 1 && reply[0] == 0x2a && atomic_load(&fx.fc_calls) == 1);
	mogbus_client_close(client);
	ASSERT_EQ(fx_stop(&fx) < STOP_BOUND_MS);
}
