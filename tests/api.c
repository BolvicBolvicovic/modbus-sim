/* Public C API (include/mogbus.h), driven in-process the way an embedding host would. */
#define _GNU_SOURCE
#include "vtest.h"
#include "build_config.h"
#include "mogbus.h"
#include "modbus/pdu.h"
#include "tests/tests_utils/test_support.h"
#include "util/clock.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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
	u64 deadline = clock_now_ms() + 2000;

	while (clock_now_ms() < deadline)
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

	ASSERT_EQ(strcmp(mogbus_version(), "0.1.0") == 0);
	ASSERT_EQ(o.mode == MOGBUS_MODE_TCP && strcmp(o.bind_address, "127.0.0.1") == 0);
	ASSERT_EQ(o.port == 1502 && o.fd == -1 && o.idle_timeout_ms == 0);
	ASSERT_EQ(o.data_bits == 8 && o.parity == MOGBUS_PARITY_NONE && o.stop_bits == 1);
	ASSERT_EQ(o.tls_require_client_certificate && o.tls_handshake_timeout_ms == 10000);
	mogbus_destroy(NULL);
}

TEST(ApiDevicesPointsAndValues)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p;
	uint16_t	words[2] = { 0, 0 };
	double		v = 0;
	char		err[128];

	ASSERT_EQ(ctx != NULL);
	dev = mogbus_add_device(ctx, 7, "pump", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_find_device(ctx, 7) == dev);
	ASSERT_EQ(mogbus_find_device(ctx, 8) == NULL);

	p = holding(0, 42);
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_INPUT, .address = 10, .type = MOGBUS_TYPE_F32, .value = 1.5,
			      .word_order = MOGBUS_WORDS_LOW_FIRST };
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_COIL, .address = 3, .type = MOGBUS_TYPE_BOOL, .value = 1 };
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 20, .type = MOGBUS_TYPE_U16,
			      .kind = MOGBUS_POINT_RAMP,
			      .ramp = { .from = 5, .to = 5, .step = 1, .every_ms = 1000, .mode = MOGBUS_RAMP_HOLD } };
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));

	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 1, words) && words[0] == 42);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_INPUT, 10, &v) && v == 1.5);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_COIL, 3, &v) && v == 1.0);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 20, &v) && v == 5.0);
	ASSERT_EQ(!mogbus_device_get_value(dev, MOGBUS_HOLDING, 1, &v));	/* undefined */
	ASSERT_EQ(!mogbus_device_read(dev, MOGBUS_HOLDING, 0, 2, words));	/* 1 is undefined */

	/* A raw write behaves like a client write; the point stays writable. */
	words[0] = 99;
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 0, 1, words));
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) && v == 99);
	words[0] = 2;
	ASSERT_EQ(!mogbus_device_write(dev, MOGBUS_COIL, 3, 1, words));	/* bits are 0/1 */

	/* set_value claims the point for the host, also on read-only spaces. */
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_INPUT, 10, -2.25));
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_INPUT, 10, &v) && v == -2.25);
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_COIL, 3, 0));
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_COIL, 3, &v) && v == 0.0);
	words[0] = 1;
	ASSERT_EQ(!mogbus_device_write(dev, MOGBUS_COIL, 3, 1, words));	/* host-owned now */
	ASSERT_EQ(!mogbus_device_set_value(dev, MOGBUS_HOLDING, 5, 1));	/* no point there */
	mogbus_destroy(ctx);
}

TEST(ApiRejectsBadInput)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p;
	char		err[128];
	char		name[100];

	ASSERT_EQ(ctx != NULL);
	dev = mogbus_add_device(ctx, 1, NULL, err, sizeof(err));
	ASSERT_EQ(dev != NULL);

	ASSERT_EQ(mogbus_add_device(ctx, 1, "again", err, sizeof(err)) == NULL);
	ASSERT_EQ(strstr(err, "unit id 1") != NULL);
	memset(name, 'x', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	ASSERT_EQ(mogbus_add_device(ctx, 2, name, err, sizeof(err)) == NULL && strstr(err, "too long"));
	ASSERT_EQ(mogbus_add_device(ctx, 2, "ok", NULL, 0) != NULL);	/* err is optional */

	p = holding(0, 1);
	p.type = MOGBUS_TYPE_BOOL;					/* bool in a register table */
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)) && err[0]);
	p = holding(65535, 1);
	p.type = MOGBUS_TYPE_U32;					/* does not fit */
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = holding(5, 1);
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)) && strstr(err, "already used"));
	p = holding(6, 0);
	p.kind = MOGBUS_POINT_RAMP;					/* step 0 */
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = holding(6, 0);
	p.value = 1.0 / 0.0;						/* infinity needs float32 */
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_COIL, .address = 1, .type = MOGBUS_TYPE_BOOL,
			      .kind = MOGBUS_POINT_RAMP, .ramp = { .step = 1, .every_ms = 1 } };
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));	/* ramping coil */
	p = holding(7, 0);
	p.space = (mogbus_space_t)9;
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));

	for (int unit = 3; unit < MOGBUS_MAX_DEVICES + 1; unit++)
		ASSERT_EQ(mogbus_add_device(ctx, (uint8_t)unit, NULL, err, sizeof(err)) != NULL);
	ASSERT_EQ(mogbus_add_device(ctx, 100, NULL, err, sizeof(err)) == NULL);
	ASSERT_EQ(strstr(err, "too many devices") != NULL);
	mogbus_destroy(ctx);
}

TEST(ApiServesDevicesOverTcpAndRejectsBadServers)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 0x1234);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;
	mogbus_server_t* other;
	char		err[256];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	int		fd;

	ASSERT_EQ(ctx != NULL);
	o.port = ts_free_port();
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err)) == NULL);	/* no devices yet */
	ASSERT_EQ(strstr(err, "no devices") != NULL);

	dev = mogbus_add_device(ctx, 1, "tcp", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, err, sizeof(err)));
	server = mogbus_server_create(ctx, &o, err, sizeof(err));
	ASSERT_EQ(server != NULL);
	ASSERT_EQ(mogbus_server_pty_name(server) == NULL);
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err)) == NULL && err[0]);	/* port in use */

	fd = ts_connect_loopback(o.port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0);
	ASSERT_EQ(ts_mbap_send(fd, 1, 7, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(server, fd));
	ASSERT_EQ(ts_mbap_recv(fd, 1, 7, resp, &len, 1000));
	ASSERT_EQ(len == 4 && resp[2] == 0x12 && resp[3] == 0x34);

	/* A host change shows up on the wire. */
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 0, 0x4321));
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
	other = mogbus_server_create(ctx, &o, err, sizeof(err));
	ASSERT_EQ(other != NULL);
	o.mode = MOGBUS_MODE_RTU_DEVICE;
	o.path = NULL;
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err)) == NULL && strstr(err, "path"));
	o.mode = MOGBUS_MODE_ASCII_FD;
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err)) == NULL && strstr(err, "fd"));
	o = mogbus_server_options_defaults();
	o.tls_enabled = true;	/* unreadable files, or a build without TLS */
	o.tls_certificate = "/nonexistent/cert.pem";
	o.tls_private_key = "/nonexistent/key.pem";
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err)) == NULL && err[0]);
	o = mogbus_server_options_defaults();
	o.bind_address = "01234567890123456789012345678901234567890123456789012345678901234567890123456789"
			 "01234567890123456789012345678901234567890123456789";
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err)) == NULL && strstr(err, "too long"));
	mogbus_destroy(ctx);	/* closes `other` */
}

#if WITH_CONFIG
TEST(ApiLoadsConfigAndServesFromIt)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	char		text[256];
	int		cfd = mkstemp(path);
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_server_t* server;
	u16		port = ts_free_port();
	char		err[256] = "";
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

	ASSERT_EQ(mogbus_server_create_from_config(ctx, err, sizeof(err)) == NULL);	/* nothing loaded */
	ASSERT_EQ(strstr(err, "no config") != NULL);
	ASSERT_EQ(!mogbus_load_config(ctx, "/nonexistent/mogbus.scm", err, sizeof(err)) && err[0]);
	ASSERT_EQ(mogbus_load_config(ctx, path, err, sizeof(err)));
	dev = mogbus_find_device(ctx, 4);
	ASSERT_EQ(dev != NULL && mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) && v == 77);

	/* Loading again would duplicate unit 4: rejected, context unchanged. */
	ASSERT_EQ(!mogbus_load_config(ctx, path, err, sizeof(err)) && strstr(err, "unit id 4"));
	ASSERT_EQ(mogbus_find_device(ctx, 4) == dev);

	/* Devices built in code are served together with the loaded ones. */
	{
		mogbus_device_t* extra = mogbus_add_device(ctx, 5, "extra", err, sizeof(err));
		mogbus_point_t p = holding(0, 5);

		ASSERT_EQ(extra != NULL && mogbus_device_add_point(extra, &p, err, sizeof(err)));
	}
	server = mogbus_server_create_from_config(ctx, err, sizeof(err));
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
	mogbus_t*	ctx = mogbus_create();
	char		err[256] = "";

	ASSERT_EQ(ctx != NULL);
	ASSERT_EQ(!mogbus_load_config(ctx, "/nonexistent/mogbus.scm", err, sizeof(err)));
	ASSERT_EQ(strcmp(err, "built without config support") == 0);
	err[0] = '\0';
	ASSERT_EQ(mogbus_server_create_from_config(ctx, err, sizeof(err)) == NULL);
	ASSERT_EQ(strcmp(err, "built without config support") == 0);
	mogbus_destroy(ctx);
}
#endif

TEST(ApiServesAnExistingFdInRtuAndAsciiModes)
{
	static const mogbus_mode_t modes[] = { MOGBUS_MODE_RTU_FD, MOGBUS_MODE_ASCII_FD };

	for (u32 i = 0; i < 2; i++)
	{
		bool		ascii = modes[i] == MOGBUS_MODE_ASCII_FD;
		mogbus_t*	ctx = mogbus_create();
		mogbus_device_t* dev = mogbus_add_device(ctx, 3, "fd", NULL, 0);
		mogbus_point_t	p = holding(0, 0xABCD);
		mogbus_server_options_t o = mogbus_server_options_defaults();
		mogbus_server_t* server;
		int		pair[2];
		u8		resp[MODBUS_MAX_PDU];
		u8		addr = 0;
		u32		len = 0;
		bool		sent;

		ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, NULL, 0));
		ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
		o.mode = modes[i];
		o.fd = pair[1];
		o.frame_gap_ms = 2;
		server = mogbus_server_create(ctx, &o, NULL, 0);
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
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev = mogbus_add_device(ctx, 1, NULL, NULL, 0);
	mogbus_point_t	p = holding(0, 1);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;

	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, NULL, 0));
	o.port = ts_free_port();
	mogbus_set_log_sink(sink_fn, &sink);
	server = mogbus_server_create(ctx, &o, NULL, 0);
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
