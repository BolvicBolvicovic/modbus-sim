/* The typed client calls of the remaining function codes, byte timeouts, host names and TLS,
 * against a library server on its own thread, plus a raw socket server for replies the library
 * never sends (malformed ones, stalls). */
#define _GNU_SOURCE
#include "vtest.h"
#include "build_config.h"
#include "tests/tests_utils/test_case.h"
#include "mogbus.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TLS_CA		"tests/tls/ca.pem"
#define TLS_CERT	"tests/tls/server.pem"
#define TLS_KEY		"tests/tls/server.key"
#define TLS_CLIENT_CERT	"tests/tls/client.pem"
#define TLS_CLIENT_KEY	"tests/tls/client.key"

typedef struct fx_t fx_t;
typedef struct raw_t raw_t;

struct fx_t
{
	mogbus_t*		ctx;
	mogbus_server_t*	server;
	mogbus_mode_t		mode;
	u16			port;
	bool			tls;		/* serve TLS with client certificates required */
	pthread_t		thread;
	atomic_bool		stop;
};

/* A server that answers each MBAP request with the next canned PDU, optionally slowly. */
struct raw_t
{
	int		listener;
	u16		port;
	pthread_t	thread;
	u8		pdus[8][260];
	u32		lens[8];
	u32		count;
	u32		first_delay_ms;	/* before the first byte of the first reply */
	u32		split_at;	/* bytes of the first reply sent before the stall; 0 = no stall */
	u32		stall_ms;
};

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
	if (fx->tls)
	{
		o.tls_enabled                    = true;
		o.tls_certificate                = TLS_CERT;
		o.tls_private_key                = TLS_KEY;
		o.tls_ca_certificate             = TLS_CA;
		o.tls_require_client_certificate = true;
	}
	mogbus_server_create(fx->ctx, &o, &fx->server);
	if (!fx->server)
		return false;
	atomic_store(&fx->stop, false);
	return pthread_create(&fx->thread, NULL, server_thread, fx) == 0;
}

/* Unit 1: holding 0-9 = 100+i, file 1 records 0-4 = 10..14, FIFO 100 = {0x1234, 0x5678, 42},
 * CANopen object node 5, index 0x2000, subindex 0 of 4 bytes. */
static bool
fx_start(fx_t* fx, mogbus_mode_t mode, bool tls)
{
	mogbus_device_t*	dev;
	const uint16_t		file[5]  = { 10, 11, 12, 13, 14 };
	const uint16_t		fifo[3]  = { 0x1234, 0x5678, 42 };
	const uint8_t		obj[4]   = { 0xDE, 0xAD, 0xBE, 0xEF };

	memset(fx, 0, sizeof(*fx));
	fx->mode = mode;
	fx->tls  = tls;
	fx->port = ts_free_port();
	mogbus_create(&fx->ctx);
	if (!fx->ctx || fx->port == 0)
		return false;
	mogbus_add_device(fx->ctx, 1, "calls", &dev);
	if (!dev)
		return false;
	for (uint32_t i = 0; i < 10; i++)
	{
		mogbus_point_t p = { .space = MOGBUS_HOLDING, .address = i, .type = MOGBUS_TYPE_U16, .value = 100 + i };

		if (mogbus_device_add_point(dev, &p) != MOGBUS_OK)
			return false;
	}
	return mogbus_device_add_file_record(dev, 1, 0, 5, file) == MOGBUS_OK &&
	       mogbus_device_add_fifo(dev, 100, 3, fifo) == MOGBUS_OK &&
	       mogbus_device_add_canopen_object(dev, 5, 0x2000, 0, 4, obj) == MOGBUS_OK &&
	       server_up(fx);
}

static void
fx_stop(fx_t* fx)
{
	if (fx->server)
	{
		atomic_store(&fx->stop, true);
		pthread_join(fx->thread, NULL);
		mogbus_server_close(fx->server);
		fx->server = NULL;
	}
	mogbus_destroy(fx->ctx);
	fx->ctx = NULL;
}

static mogbus_client_t*
client_for(const fx_t* fx)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();
	mogbus_client_t*	c = NULL;

	o.mode       = fx->mode;
	o.port       = fx->port;
	o.timeout_ms = 1000;
	mogbus_client_open(&o, &c);
	return c;
}

/* Each of the calls, in whatever framing the client uses. */
static void
exercise_remaining_calls(mogbus_client_t* c)
{
	uint8_t		status8 = 99;
	uint16_t	status = 1;
	uint16_t	count = 0;
	uint16_t	messages = 0;
	uint8_t		buf[260];
	size_t		n = 0;

	ASSERT_EQ(mogbus_client_read_exception_status(c, 1, &status8) == MOGBUS_OK && status8 == 0);

	{
		const uint8_t	echo[6] = { 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45 };

		/* Sub-function 0 echoes data of any length (the stream framer must not assume 2). */
		ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0x0000, echo, sizeof(echo), buf, sizeof(buf), &n) == MOGBUS_OK);
		ASSERT_EQ(n == sizeof(echo) && memcmp(buf, echo, n) == 0);
		ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0x000A, (const uint8_t[]){ 0, 0 }, 2, buf, sizeof(buf), &n) == MOGBUS_OK);
		ASSERT_EQ(n == 2 && buf[0] == 0 && buf[1] == 0);
		ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0x000B, (const uint8_t[]){ 0, 0 }, 2, buf, sizeof(buf), &n) == MOGBUS_OK);
		ASSERT_EQ(n == 2);
		ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0x0003, (const uint8_t[]){ 0x0A, 0 }, 2, buf, sizeof(buf), &n) == MOGBUS_ERR_EXCEPTION);
		ASSERT_EQ(mogbus_client_last_exception(c) == 3);
	}

	ASSERT_EQ(mogbus_client_get_comm_event_counter(c, 1, &status, &count) == MOGBUS_OK && status == 0);
	ASSERT_EQ(mogbus_client_get_comm_event_log(c, 1, &status, &count, &messages, buf, 64, &n) == MOGBUS_OK);
	ASSERT_EQ(status == 0 && n == 0 && messages >= 1);

	ASSERT_EQ(mogbus_client_report_server_id(c, 1, buf, sizeof(buf), &n) == MOGBUS_OK);
	ASSERT_EQ(n == 2 && buf[0] == 0 && buf[1] == 0xFF);

	{
		uint16_t		a[2] = { 0, 0 };
		uint16_t		b[1] = { 0 };
		mogbus_client_file_record_t two[2] = { { .file = 1, .record = 0, .length = 2, .values = a },
						       { .file = 1, .record = 4, .length = 1, .values = b } };

		ASSERT_EQ(mogbus_client_read_file_records(c, 1, two, 2) == MOGBUS_OK);
		ASSERT_EQ(a[0] == 10 && a[1] == 11 && b[0] == 14);

		a[0] = 0xAAAA;
		a[1] = 0xBBBB;
		ASSERT_EQ(mogbus_client_write_file_records(c, 1, &two[0], 1) == MOGBUS_OK);
		a[0] = a[1] = 0;
		ASSERT_EQ(mogbus_client_read_file_records(c, 1, &two[0], 1) == MOGBUS_OK);
		ASSERT_EQ(a[0] == 0xAAAA && a[1] == 0xBBBB);

		two[1].file = 9;	/* not configured */
		ASSERT_EQ(mogbus_client_read_file_records(c, 1, two, 2) == MOGBUS_ERR_EXCEPTION);
		ASSERT_EQ(mogbus_client_last_exception(c) == 2);
		ASSERT_EQ(mogbus_client_write_file_records(c, 1, &two[1], 1) == MOGBUS_ERR_EXCEPTION);
		ASSERT_EQ(mogbus_client_last_exception(c) == 2);
	}

	{
		uint16_t v[31];

		ASSERT_EQ(mogbus_client_read_fifo(c, 1, 100, v, 31, &n) == MOGBUS_OK);
		ASSERT_EQ(n == 3 && v[0] == 0x1234 && v[1] == 0x5678 && v[2] == 42);
		ASSERT_EQ(mogbus_client_read_fifo(c, 1, 101, v, 31, &n) == MOGBUS_ERR_EXCEPTION);
		ASSERT_EQ(mogbus_client_last_exception(c) == 2);
		ASSERT_EQ(mogbus_client_read_fifo(c, 1, 100, v, 2, &n) == MOGBUS_ERR_FRAME);
	}

	{
		/* read 4 bytes of node 5, index 0x2000:0 from offset 0 */
		const uint8_t	read[]  = { 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
		const uint8_t	write[] = { 0x01, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x11, 0x22 };
		const uint8_t	absent[] = { 0x00, 0x00, 0x06, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
		uint8_t		reply[260];

		ASSERT_EQ(mogbus_client_canopen_request(c, 1, read, sizeof(read), reply, sizeof(reply), &n) == MOGBUS_OK);
		ASSERT_EQ(n == 14 && reply[0] == 0x00 && reply[2] == 0x05 && memcmp(reply + 10, "\xDE\xAD\xBE\xEF", 4) == 0);
		ASSERT_EQ(mogbus_client_canopen_request(c, 1, write, sizeof(write), reply, sizeof(reply), &n) == MOGBUS_OK);
		ASSERT_EQ(n == 10 && reply[0] == 0x01);
		ASSERT_EQ(mogbus_client_canopen_request(c, 1, read, sizeof(read), reply, sizeof(reply), &n) == MOGBUS_OK);
		ASSERT_EQ(memcmp(reply + 10, "\x11\x22\xBE\xEF", 4) == 0);

		/* A CANopen failure is AB FF len 0D CE <code, little endian>: code 0xFFFF000B. */
		ASSERT_EQ(mogbus_client_canopen_request(c, 1, absent, sizeof(absent), reply, sizeof(reply), &n) == MOGBUS_ERR_EXCEPTION);
		ASSERT_EQ(mogbus_client_last_exception(c) == 0xFF);
		ASSERT_EQ(n == 9 && reply[0] == 0xFF && reply[3] == 0x0D && reply[4] == 0xCE);
		ASSERT_EQ(memcmp(reply + 5, "\x0B\x00\xFF\xFF", 4) == 0);
	}

	{
		uint8_t level = 0;

		ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 4, 0, buf, sizeof(buf), &n, &level) == MOGBUS_OK);
		ASSERT_EQ(n == 8 && buf[0] == 0 && buf[1] == 6 && memcmp(buf + 2, "mogbus", 6) == 0 && level == 0x81);
		ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 3, 0, buf, sizeof(buf), &n, NULL) == MOGBUS_OK);
		ASSERT_EQ(n >= 3 && buf[0] == 0);
		ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 4, 0x90, buf, sizeof(buf), &n, NULL) == MOGBUS_ERR_EXCEPTION);
		ASSERT_EQ(mogbus_client_last_exception(c) == 2);
	}
}

TEST(ClientRemainingCallsOverNetworkModes)
{
	static const mogbus_mode_t modes[] = { MOGBUS_MODE_TCP, MOGBUS_MODE_UDP, MOGBUS_MODE_RTU_TCP,
					       MOGBUS_MODE_ASCII_TCP, MOGBUS_MODE_RTU_UDP,
					       MOGBUS_MODE_ASCII_UDP };

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
	{
		fx_t			fx;
		mogbus_client_t*	c;
		char			label[32];

		snprintf(label, sizeof(label), "mode %d", (int)modes[i]);
		test_case = label;
		ASSERT_EQ(fx_start(&fx, modes[i], false));
		c = client_for(&fx);
		ASSERT_EQ(c != NULL);
		exercise_remaining_calls(c);
		mogbus_client_close(c);
		fx_stop(&fx);
	}
	test_case = 0;
}

TEST(ClientRemainingCallsArgumentChecks)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();
	mogbus_client_t*	c = NULL;
	uint8_t			buf[300];
	uint16_t		v[40];
	size_t			n = 0;
	uint8_t			s8;
	uint16_t		a, b, d;

	mogbus_client_open(&o, &c);
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_read_exception_status(c, 0, &s8) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_read_exception_status(c, 1, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0, buf, 251, buf, 4, &n) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0, NULL, 2, buf, 4, &n) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_get_comm_event_counter(c, 1, NULL, &a) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_get_comm_event_log(c, 0, &a, &b, &d, buf, 64, &n) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_report_server_id(c, 1, buf, 4, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_read_fifo(c, 1, 0, v, 31, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_canopen_request(c, 1, buf, 0, buf, 4, &n) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_canopen_request(c, 1, buf, 252, buf, 4, &n) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 0, 0, buf, 4, &n, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 5, 0, buf, 4, &n, NULL) == MOGBUS_ERR_INVALID);

	{
		mogbus_client_file_record_t ok   = { .file = 1, .record = 0, .length = 2, .values = v };
		mogbus_client_file_record_t bad[] = {
			{ .file = 0, .record = 0, .length = 1, .values = v },
			{ .file = 1, .record = 10000, .length = 1, .values = v },
			{ .file = 1, .record = 9999, .length = 2, .values = v },
			{ .file = 1, .record = 0, .length = 0, .values = v },
			{ .file = 1, .record = 0, .length = 1, .values = NULL },
		};
		uint16_t		    wide[100] = { 0 };
		mogbus_client_file_record_t big[2] = { { .file = 1, .record = 0, .length = 100, .values = wide },
						       { .file = 1, .record = 200, .length = 100, .values = wide } };

		ASSERT_EQ(mogbus_client_read_file_records(c, 1, &ok, 0) == MOGBUS_ERR_INVALID);
		ASSERT_EQ(mogbus_client_read_file_records(c, 1, NULL, 1) == MOGBUS_ERR_INVALID);
		ASSERT_EQ(mogbus_client_read_file_records(c, 1, &ok, 36) == MOGBUS_ERR_INVALID);
		ASSERT_EQ(mogbus_client_write_file_records(c, 1, &ok, 0) == MOGBUS_ERR_INVALID);
		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		{
			ASSERT_EQ(mogbus_client_read_file_records(c, 1, &bad[i], 1) == MOGBUS_ERR_INVALID);
			ASSERT_EQ(mogbus_client_write_file_records(c, 1, &bad[i], 1) == MOGBUS_ERR_INVALID);
		}
		/* 2 x (7 + 200) bytes do not fit one request */
		ASSERT_EQ(mogbus_client_write_file_records(c, 1, big, 2) == MOGBUS_ERR_INVALID);
	}
	mogbus_client_close(c);
}

/* ---- raw server ---- */

static bool
read_full(int fd, u8* buf, u32 len)
{
	u32 got = 0;

	while (got < len)
	{
		ssize_t n = recv(fd, buf + got, len - got, 0);

		if (n <= 0)
			return false;
		got += (u32)n;
	}
	return true;
}

static void*
raw_thread(void* arg)
{
	raw_t*	r = arg;
	int	fd = accept(r->listener, NULL, NULL);

	if (fd < 0)
		return NULL;
	for (u32 i = 0; i < r->count; i++)
	{
		u8	head[7];
		u8	rest[260];
		u8	frame[7 + 260];
		u32	len = r->lens[i];

		if (!read_full(fd, head, sizeof(head)))
			break;
		if (!read_full(fd, rest, (u32)((head[4] << 8) | head[5]) - 1))
			break;
		memcpy(frame, head, 4);
		frame[4] = (u8)((len + 1) >> 8);
		frame[5] = (u8)(len + 1);
		frame[6] = head[6];
		memcpy(frame + 7, r->pdus[i], len);
		if (i == 0 && r->first_delay_ms)
			usleep(r->first_delay_ms * 1000);
		if (i == 0 && r->split_at)
		{
			send(fd, frame, r->split_at, MSG_NOSIGNAL);
			usleep(r->stall_ms * 1000);
			send(fd, frame + r->split_at, 7 + len - r->split_at, MSG_NOSIGNAL);
		}
		else
			send(fd, frame, 7 + len, MSG_NOSIGNAL);
	}
	{
		u8 sink[16];

		/* Wait for the client to hang up so the replies are never cut short by a reset. */
		while (recv(fd, sink, sizeof(sink), 0) > 0)
			;
	}
	close(fd);
	return NULL;
}

static bool
raw_start(raw_t* r)
{
	struct sockaddr_in	addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t		len  = sizeof(addr);

	r->listener = socket(AF_INET, SOCK_STREAM, 0);
	if (r->listener < 0 ||
	    bind(r->listener, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
	    listen(r->listener, 1) != 0 ||
	    getsockname(r->listener, (struct sockaddr*)&addr, &len) != 0)
		return false;
	r->port = ntohs(addr.sin_port);
	return pthread_create(&r->thread, NULL, raw_thread, r) == 0;
}

static void
raw_stop(raw_t* r)
{
	pthread_join(r->thread, NULL);
	close(r->listener);
}

static void
raw_reply(raw_t* r, const u8* pdu, u32 len)
{
	memcpy(r->pdus[r->count], pdu, len);
	r->lens[r->count++] = len;
}

static mogbus_client_t*
raw_client(const raw_t* r, uint32_t timeout_ms, uint32_t byte_timeout_ms)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();
	mogbus_client_t*	c = NULL;

	o.port            = r->port;
	o.timeout_ms      = timeout_ms;
	o.byte_timeout_ms = byte_timeout_ms;
	mogbus_client_open(&o, &c);
	return c;
}

TEST(ClientRepliesAreValidatedStrictly)
{
	static const u8 bad_status[] = { 0x07, 0x01, 0x02 };
	static const u8 bad_diag_sub[] = { 0x08, 0x00, 0x01, 0x00, 0x00 };
	static const u8 bad_diag_len[] = { 0x08, 0x00, 0x0A, 0x00 };
	static const u8 bad_counter[] = { 0x0B, 0x00, 0x00, 0x00 };
	static const u8 bad_log[] = { 0x0C, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0xAA };
	static const u8 bad_server_id[] = { 0x11, 0x04, 0x00, 0xFF };
	static const u8 bad_file_type[] = { 0x14, 0x04, 0x03, 0x07, 0x00, 0x01 };
	static const u8 bad_file_len[] = { 0x14, 0x05, 0x04, 0x06, 0x00, 0x01, 0x00 };
	static const u8 bad_fifo[] = { 0x18, 0x00, 0x08, 0x00, 0x01, 0x00, 0x01 };
	static const u8 bad_canopen[] = { 0x2B, 0x0D, 0x09, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const u8 more_stuck[] = { 0x2B, 0x0E, 0x01, 0x81, 0xFF, 0x00, 0x01, 0x00, 0x01, 'a' };
	static const u8 bad_objects[] = { 0x2B, 0x0E, 0x01, 0x81, 0x00, 0x00, 0x02, 0x00, 0x01, 'a' };
	raw_t		r;
	mogbus_client_t* c;
	uint8_t		s8 = 0;
	uint8_t		buf[64];
	uint16_t	w[8];
	uint16_t	x, y, z;
	size_t		n = 0;
	mogbus_client_file_record_t rec = { .file = 1, .record = 0, .length = 1, .values = w };
	const uint8_t	canopen[] = { 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	memset(&r, 0, sizeof(r));
	raw_reply(&r, bad_status, sizeof(bad_status));
	raw_reply(&r, bad_diag_sub, sizeof(bad_diag_sub));
	raw_reply(&r, bad_diag_len, sizeof(bad_diag_len));
	raw_reply(&r, bad_counter, sizeof(bad_counter));
	raw_reply(&r, bad_log, sizeof(bad_log));
	raw_reply(&r, bad_server_id, sizeof(bad_server_id));
	raw_reply(&r, bad_file_type, sizeof(bad_file_type));
	raw_reply(&r, bad_file_len, sizeof(bad_file_len));
	ASSERT_EQ(raw_start(&r));
	c = raw_client(&r, 1000, 0);
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_read_exception_status(c, 1, &s8) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0x000A, (const uint8_t[]){ 0, 0 }, 2, buf, sizeof(buf), &n) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_diagnostics(c, 1, 0x000A, (const uint8_t[]){ 0, 0 }, 2, buf, sizeof(buf), &n) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_get_comm_event_counter(c, 1, &x, &y) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_get_comm_event_log(c, 1, &x, &y, &z, buf, 64, &n) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_report_server_id(c, 1, buf, sizeof(buf), &n) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_read_file_records(c, 1, &rec, 1) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_read_file_records(c, 1, &rec, 1) == MOGBUS_ERR_FRAME);
	mogbus_client_close(c);
	raw_stop(&r);

	memset(&r, 0, sizeof(r));
	raw_reply(&r, bad_fifo, sizeof(bad_fifo));
	raw_reply(&r, bad_canopen, sizeof(bad_canopen));
	raw_reply(&r, more_stuck, sizeof(more_stuck));
	raw_reply(&r, more_stuck, sizeof(more_stuck));
	raw_reply(&r, bad_objects, sizeof(bad_objects));
	ASSERT_EQ(raw_start(&r));
	c = raw_client(&r, 1000, 0);
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_read_fifo(c, 1, 100, w, 8, &n) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_canopen_request(c, 1, canopen, sizeof(canopen), buf, sizeof(buf), &n) == MOGBUS_ERR_FRAME);
	/* "more follows" pointing at an object not after the last one is a loop */
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 1, 0, buf, sizeof(buf), &n, NULL) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 4, 0, buf, sizeof(buf), &n, NULL) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 1, 0, buf, sizeof(buf), &n, NULL) == MOGBUS_ERR_FRAME);
	mogbus_client_close(c);
	raw_stop(&r);
}

/* ---- byte timeout ---- */

TEST(ClientByteTimeout)
{
	static const u8 reply[] = { 0x03, 0x02, 0x00, 0x2A };
	raw_t		r;
	mogbus_client_t* c;
	uint16_t	v = 0;
	u64		start;

	/* A gap inside the reply longer than the byte timeout fails at once, well before timeout_ms. */
	memset(&r, 0, sizeof(r));
	raw_reply(&r, reply, sizeof(reply));
	r.split_at = 9;
	r.stall_ms = 600;
	ASSERT_EQ(raw_start(&r));
	c = raw_client(&r, 3000, 100);
	ASSERT_EQ(c != NULL);
	start = os_now_ms();
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, &v) == MOGBUS_ERR_TIMEOUT);
	ASSERT_EQ(os_now_ms() - start < 450);
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "stalled") != NULL);
	mogbus_client_close(c);
	raw_stop(&r);

	/* Without it, or with a longer one, the slow reply is accepted. */
	for (uint32_t byte_timeout = 0; byte_timeout <= 1000; byte_timeout += 1000)
	{
		memset(&r, 0, sizeof(r));
		raw_reply(&r, reply, sizeof(reply));
		r.split_at = 9;
		r.stall_ms = 200;
		ASSERT_EQ(raw_start(&r));
		c = raw_client(&r, 3000, byte_timeout);
		ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, &v) == MOGBUS_OK && v == 0x2A);
		mogbus_client_close(c);
		raw_stop(&r);
	}

	/* The byte timeout only starts with the first byte: a late first byte is the overall timeout's. */
	memset(&r, 0, sizeof(r));
	raw_reply(&r, reply, sizeof(reply));
	r.first_delay_ms = 250;
	ASSERT_EQ(raw_start(&r));
	c = raw_client(&r, 3000, 50);
	v = 0;
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, &v) == MOGBUS_OK && v == 0x2A);
	mogbus_client_close(c);
	raw_stop(&r);
}

/* ---- host names ---- */

TEST(ClientHostNames)
{
	fx_t			fx;
	mogbus_client_options_t	o = mogbus_client_options_defaults();
	mogbus_client_t*	c = NULL;
	uint16_t		v = 0;

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP, false));
	o.host       = "localhost";	/* may resolve to ::1 first: the next address is tried */
	o.port       = fx.port;
	o.timeout_ms = 1000;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 3, 1, &v) == MOGBUS_OK && v == 103);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 4, 1, &v) == MOGBUS_OK && v == 104);
	mogbus_client_close(c);

	o.host = "LocalHost";
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 3, 1, &v) == MOGBUS_OK && v == 103);
	mogbus_client_close(c);

	/* Resolution happens at connect time: opening a client for an unknown name works. */
	o.host = "no-such-host.invalid";
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_connect(c) == MOGBUS_ERR_IO);
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "no-such-host.invalid") != NULL);
	mogbus_client_close(c);

	o.host = "bad host!";
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_ERR_INVALID && c == NULL);
	o.host = "";
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_ERR_INVALID);
	fx_stop(&fx);
}

/* ---- TLS ---- */

#if WITH_TLS

static mogbus_client_options_t
tls_options(const fx_t* fx)
{
	mogbus_client_options_t o = mogbus_client_options_defaults();

	o.port          = fx->port;
	o.tls_enabled   = true;
	o.tls_ca_file   = TLS_CA;
	o.tls_cert_file = TLS_CLIENT_CERT;
	o.tls_key_file  = TLS_CLIENT_KEY;
	o.timeout_ms    = 2000;
	return o;
}

TEST(ClientTlsMutualAuthentication)
{
	fx_t			fx;
	mogbus_client_options_t	o;
	mogbus_client_t*	c = NULL;

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP, true));
	o = tls_options(&fx);
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	exercise_remaining_calls(c);
	mogbus_client_close(c);

	/* The certificate names 127.0.0.1 and localhost: both reach it, the name is checked. */
	o.host = "localhost";
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_connect(c) == MOGBUS_OK);
	mogbus_client_close(c);

	/* No key file: the key is looked for in the certificate file, which holds none. */
	o.host         = "127.0.0.1";
	o.tls_key_file = NULL;
	ASSERT_EQ(mogbus_client_open(&o, &c) != MOGBUS_OK && c == NULL);
	ASSERT_EQ(strstr(mogbus_last_error(NULL), "TLS") != NULL);
	fx_stop(&fx);
}

TEST(ClientTlsRejectsWhatItCannotTrust)
{
	fx_t			fx;
	mogbus_client_options_t	o;
	mogbus_client_t*	c = NULL;
	uint16_t		v = 0;

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP, true));

	/* A trust anchor that did not sign the server certificate. */
	o = tls_options(&fx);
	o.tls_ca_file = TLS_CLIENT_CERT;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, &v) == MOGBUS_ERR_IO);
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "certificate verify failed") != NULL);
	mogbus_client_close(c);

	/* A name the certificate does not carry. */
	o = tls_options(&fx);
	o.tls_server_name = "wrong.example";
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, &v) == MOGBUS_ERR_IO);
	ASSERT_EQ(strstr(mogbus_client_last_error(c), "hostname mismatch") != NULL);
	mogbus_client_close(c);

	/* ... which is fine when verification is off. */
	o.tls_verify = false;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, &v) == MOGBUS_OK && v == 100);
	mogbus_client_close(c);

	/* The server requires a client certificate and this client has none. */
	o = tls_options(&fx);
	o.tls_cert_file = NULL;
	o.tls_key_file  = NULL;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	ASSERT_EQ(mogbus_client_read_holding_registers(c, 1, 0, 1, &v) == MOGBUS_ERR_IO);
	ASSERT_EQ(mogbus_client_last_error(c)[0] != '\0');
	mogbus_client_close(c);

	/* Missing or invalid files fail the open. */
	o = tls_options(&fx);
	o.tls_ca_file = "/nonexistent/ca.pem";
	ASSERT_EQ(mogbus_client_open(&o, &c) != MOGBUS_OK && c == NULL);
	o = tls_options(&fx);
	o.mode = MOGBUS_MODE_RTU_TCP;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(strstr(mogbus_last_error(NULL), "tcp mode") != NULL);
	fx_stop(&fx);
}

TEST(ClientTlsAgainstPlainServerFailsCleanly)
{
	fx_t			fx;
	mogbus_client_options_t	o;
	mogbus_client_t*	c = NULL;
	uint16_t		v = 0;
	mogbus_result_t		r;

	ASSERT_EQ(fx_start(&fx, MOGBUS_MODE_TCP, false));
	o = tls_options(&fx);
	o.timeout_ms = 500;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	r = mogbus_client_read_holding_registers(c, 1, 0, 1, &v);
	ASSERT_EQ(r == MOGBUS_ERR_IO || r == MOGBUS_ERR_TIMEOUT);
	mogbus_client_close(c);
	fx_stop(&fx);
}

TEST(ClientTlsDefaultPort)
{
	mogbus_client_options_t	o = mogbus_client_options_defaults();
	mogbus_client_t*	c = NULL;

	o.port        = 0;
	o.tls_enabled = true;
	o.tls_ca_file = TLS_CA;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_OK);
	mogbus_client_close(c);
	o.tls_enabled = false;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_ERR_INVALID);
}

TEST(ProbeEchoesOverTls)
{
	fx_t			fx;
	fx_t			plain;
	mogbus_server_options_t	o = mogbus_server_options_defaults();
	mogbus_device_t*	dev;

	/* A TLS server without client certificates: a real FC 08 echo over TLS. */
	memset(&fx, 0, sizeof(fx));
	fx.port = ts_free_port();
	mogbus_create(&fx.ctx);
	ASSERT_EQ(fx.ctx != NULL);
	mogbus_add_device(fx.ctx, 1, "probe", &dev);
	ASSERT_EQ(dev != NULL);
	o.mode            = MOGBUS_MODE_TCP;
	o.port            = fx.port;
	o.tls_enabled     = true;
	o.tls_certificate = TLS_CERT;
	o.tls_private_key = TLS_KEY;
	o.tls_require_client_certificate = false;
	mogbus_server_create(fx.ctx, &o, &fx.server);
	ASSERT_EQ(fx.server != NULL);
	atomic_store(&fx.stop, false);
	ASSERT_EQ(pthread_create(&fx.thread, NULL, server_thread, &fx) == 0);
	ASSERT_EQ(mogbus_probe(&o, 1, 2000) == MOGBUS_OK);
	/* An unknown unit is an exception reply, which still shows the server alive. */
	ASSERT_EQ(mogbus_probe(&o, 9, 2000) == MOGBUS_OK);
	fx_stop(&fx);

	/* A plain server on the port: connecting is not enough. */
	ASSERT_EQ(fx_start(&plain, MOGBUS_MODE_TCP, false));
	o.port = plain.port;
	ASSERT_EQ(mogbus_probe(&o, 1, 500) == MOGBUS_ERR_PROBE);
	ASSERT_EQ(mogbus_last_error(NULL)[0] != '\0');
	/* With client certificates required the probe has none and only connects. */
	o.tls_require_client_certificate = true;
	ASSERT_EQ(mogbus_probe(&o, 1, 500) == MOGBUS_OK);
	fx_stop(&plain);
}

#else

TEST(ClientTlsNeedsATlsBuild)
{
	mogbus_client_options_t	o = mogbus_client_options_defaults();
	mogbus_client_t*	c = NULL;

	o.tls_enabled = true;
	ASSERT_EQ(mogbus_client_open(&o, &c) == MOGBUS_ERR_UNSUPPORTED && c == NULL);
	ASSERT_EQ(strstr(mogbus_last_error(NULL), "without TLS") != NULL);
}

#endif

/* ---- calls that need values only a config can set ---- */

#if WITH_CONFIG

static void
walk_ids(const uint8_t* objects, size_t len, uint8_t* ids, size_t* count)
{
	size_t i = 0;

	*count = 0;
	while (i + 2 <= len && i + 2 + objects[i + 1] <= len)
	{
		ids[(*count)++] = objects[i];
		i += 2u + objects[i + 1];
	}
	ASSERT_EQ(i == len);
}

TEST(ClientConfiguredValuesAndDeviceIdFollowUp)
{
	char			path[64];
	char			text[2048];
	char			big[3][201];
	FILE*			f;
	u16			port = ts_free_port();
	mogbus_t*		ctx = NULL;
	mogbus_server_t*	server = NULL;
	fx_t			fx;
	mogbus_client_t*	c;
	uint8_t			s8 = 0;
	uint8_t			buf[1024];
	uint8_t			ids[16];
	uint8_t			level = 0;
	uint16_t		status, events, messages;
	size_t			n = 0;
	size_t			id_count = 0;

	for (int i = 0; i < 3; i++)
	{
		memset(big[i], 'A' + i, 200);
		big[i][200] = '\0';
	}
	snprintf(text, sizeof(text),
		 "(server (mode tcp) (port %u))\n"
		 "(device (unit-id 1) (exception-status 5) (server-id 7) (run-indicator 0)\n"
		 "  (server-id-additional-data \"AABB\") (com-status 65535) (com-event 128) (com-event 255)\n"
		 "  (device-id-object 128 \"%s\") (device-id-object 129 \"%s\") (device-id-object 130 \"%s\")\n"
		 "  (holding 0 (static 1)))\n",
		 port, big[0], big[1], big[2]);
	ASSERT_EQ(ts_temp_file(path, sizeof(path), "/tmp/mogbus-calls-XXXXXX"));
	f = fopen(path, "w");
	ASSERT_EQ(f != NULL);
	fputs(text, f);
	fclose(f);

	ASSERT_EQ(mogbus_create(&ctx) == MOGBUS_OK);
	ASSERT_EQ(mogbus_load_config(ctx, path) == MOGBUS_OK);
	unlink(path);
	ASSERT_EQ(mogbus_server_create_from_config(ctx, &server) == MOGBUS_OK);
	memset(&fx, 0, sizeof(fx));
	fx.ctx    = ctx;
	fx.server = server;
	atomic_store(&fx.stop, false);
	ASSERT_EQ(pthread_create(&fx.thread, NULL, server_thread, &fx) == 0);

	{
		mogbus_client_options_t o = mogbus_client_options_defaults();

		o.port = port;
		mogbus_client_open(&o, &c);
	}
	ASSERT_EQ(c != NULL);
	ASSERT_EQ(mogbus_client_read_exception_status(c, 1, &s8) == MOGBUS_OK && s8 == 5);
	ASSERT_EQ(mogbus_client_report_server_id(c, 1, buf, sizeof(buf), &n) == MOGBUS_OK);
	ASSERT_EQ(n == 4 && buf[0] == 7 && buf[1] == 0 && buf[2] == 0xAA && buf[3] == 0xBB);
	ASSERT_EQ(mogbus_client_get_comm_event_counter(c, 1, &status, &events) == MOGBUS_OK && status == 0xFFFF);
	ASSERT_EQ(mogbus_client_get_comm_event_log(c, 1, &status, &events, &messages, buf, 64, &n) == MOGBUS_OK);
	ASSERT_EQ(status == 0xFFFF && n == 2 && buf[0] == 128 && buf[1] == 255);

	/* 627 bytes of objects over several replies, joined in order. */
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 3, 0, buf, sizeof(buf), &n, &level) == MOGBUS_OK);
	walk_ids(buf, n, ids, &id_count);
	ASSERT_EQ(n > 253 && level == 0x83);
	ASSERT_EQ(id_count == 6 && ids[0] == 0 && ids[1] == 1 && ids[2] == 2 && ids[3] == 128 &&
		  ids[4] == 129 && ids[5] == 130);
	/* Starting later starts later. */
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 3, 129, buf, sizeof(buf), &n, NULL) == MOGBUS_OK);
	walk_ids(buf, n, ids, &id_count);
	ASSERT_EQ(id_count == 2 && ids[0] == 129 && ids[1] == 130);
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 2, 0, buf, sizeof(buf), &n, NULL) == MOGBUS_OK);
	walk_ids(buf, n, ids, &id_count);
	ASSERT_EQ(id_count == 3);
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 4, 130, buf, sizeof(buf), &n, NULL) == MOGBUS_OK);
	ASSERT_EQ(n == 202 && buf[0] == 130 && buf[1] == 200);
	/* The buffer is too small for all the replies. */
	ASSERT_EQ(mogbus_client_read_device_identification(c, 1, 3, 0, buf, 300, &n, NULL) == MOGBUS_ERR_FRAME);
	/* The old call is basic only, and still works. */
	ASSERT_EQ(mogbus_client_read_device_id(c, 1, buf, sizeof(buf), &n) == MOGBUS_OK);
	walk_ids(buf, n, ids, &id_count);
	ASSERT_EQ(id_count == 3);
	mogbus_client_close(c);

	fx.ctx = ctx;
	fx_stop(&fx);
}

#endif
