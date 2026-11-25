/* Server facade: one transport-independent init/step/stop/close API, driven in-process
 * with devices built in code (no config), plus the config -> options adapter. */
#define _GNU_SOURCE
#include "vtest.h"
#include "build_config.h"
#if WITH_CONFIG
#include "config/config.h"
#endif
#include "device/device.h"
#include "modbus/pdu.h"
#include "server/server.h"
#if WITH_CONFIG
#include "server/server_config.h"
#endif
#include "tests/tests_utils/test_support.h"
#include "util/clock.h"
#include "vlib/arena.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static device_t*
make_device(arena_t* arena, u8 unit, u16 value)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { 0 };

	if (!dev)
		return NULL;
	dev->unit_id = unit;
	strcpy(dev->name, "facade");
	p.space = SPACE_HOLDING;
	p.type = VT_U16;
	p.kind = POINT_STATIC;
	p.static_value = (f64)value;
	return device_add_point(dev, &p) == NULL ? dev : NULL;
}

/* Steps the server until fd is readable. */
static bool
pump(server_t* server, int fd)
{
	u64 deadline = clock_now_ms() + 2000;

	while (clock_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		(void)server_step(server, 1);
		if (poll(&pfd, 1, 0) > 0)
			return true;
	}
	return false;
}

static const u8 READ_HOLDING[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };

TEST(ServerFacadeTcpServesOneRequestThenStops)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, 0x1234);
	server_options_t o = {
		.mode = SERVER_MODE_TCP,
		.bind_address = "127.0.0.1",
		.port = ts_free_port(),
		.seed = 1,
		.devices = &dev,
		.device_count = 1,
	};
	server_t	s;
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	int		fd;

	ASSERT_EQ(dev != NULL && o.port != 0);
	ASSERT_EQ(server_init(&s, &o) == 0);
	ASSERT_EQ(server_pty_name(&s) == NULL);
	fd = ts_connect_loopback(o.port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0);
	ASSERT_EQ(ts_mbap_send(fd, 1, 7, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(&s, fd));
	ASSERT_EQ(ts_mbap_recv(fd, 1, 7, resp, &len, 1000));
	ASSERT_EQ(len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(resp[2] == 0x12 && resp[3] == 0x34);

	server_stop(&s);
	ASSERT_EQ(server_step(&s, 0) == NET_STEP_DONE);
	server_close(&s);
	server_close(&s);
	close(fd);

	/* The listener is gone: the port can be taken again, and a busy port reports 2. */
	ASSERT_EQ(server_init(&s, &o) == 0);
	{
		server_t other;

		ASSERT_EQ(server_init(&other, &o) == 2);
	}
	server_close(&s);
	arena_release(arena);
}

TEST(ServerFacadeServesAnExistingFdInRtuAndAsciiModes)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 3, 0xABCD);
	static const server_mode_t modes[] = { SERVER_MODE_RTU_FD, SERVER_MODE_ASCII_FD };

	ASSERT_EQ(dev != NULL);
	for (u32 i = 0; i < 2; i++)
	{
		bool		ascii = modes[i] == SERVER_MODE_ASCII_FD;
		int		pair[2];
		server_t	s;
		server_options_t o = {
			.mode = modes[i],
			.seed = 1,
			.frame_gap_ms = 2,
			.devices = &dev,
			.device_count = 1,
		};
		u8		resp[MODBUS_MAX_PDU];
		u8		addr = 0;
		u32		len = 0;
		bool		sent;

		ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
		o.fd = pair[1];
		ASSERT_EQ(server_init(&s, &o) == 0);
		ASSERT_EQ(server_pty_name(&s) == NULL);
		sent = ascii ? ts_ascii_send(pair[0], 3, READ_HOLDING, sizeof(READ_HOLDING), true) :
			       ts_rtu_send(pair[0], 3, READ_HOLDING, sizeof(READ_HOLDING), true);
		ASSERT_EQ(sent);
		ASSERT_EQ(pump(&s, pair[0]));
		if (ascii)
			ASSERT_EQ(ts_ascii_recv_response(pair[0], &addr, resp, &len, 500, true));
		else
			ASSERT_EQ(ts_rtu_recv(pair[0], &addr, resp, &len, 500, true));
		ASSERT_EQ(addr == 3 && len == 4 && resp[2] == 0xAB && resp[3] == 0xCD);

		server_stop(&s);
		ASSERT_EQ(server_serve(&s) == 0);	/* returns at once, closes */
		/* The fd stays the host's: the peer sees no EOF until the host closes it. */
		ASSERT_EQ(write(pair[1], "x", 1) == 1);
		close(pair[0]);
		close(pair[1]);
	}
	arena_release(arena);
}

TEST(ServerFacadeOpensPtyAndReportsSlaveName)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, 1);
	server_t	s;
	server_options_t o = {
		.mode = SERVER_MODE_RTU_PTY,
		.stop_bits = 1,
		.devices = &dev,
		.device_count = 1,
	};
	const char*	name;

	ASSERT_EQ(dev != NULL);
	ASSERT_EQ(server_init(&s, &o) == 0);
	name = server_pty_name(&s);
	ASSERT_EQ(name != NULL && strncmp(name, "/dev/pts/", 9) == 0);
	ASSERT_EQ(access(name, F_OK) == 0);
	ASSERT_EQ(server_step(&s, 0) == NET_STEP_CONTINUE);
	server_close(&s);
	server_close(&s);
	ASSERT_EQ(server_pty_name(&s) == NULL);

	o.mode = SERVER_MODE_ASCII_DEVICE;
	o.path = "/nonexistent/mogbus-tty";
	o.data_bits = 8;
	ASSERT_EQ(server_init(&s, &o) == 1);
	arena_release(arena);
}

TEST(ServerFacadeCheckTlsOnlyValidatesWhenEnabled)
{
	server_options_t o = { .mode = SERVER_MODE_TCP };
	char		err[256] = "";

	ASSERT_EQ(server_check_tls(&o, err, sizeof(err)));
	o.tls_enabled = true;
	o.tls.certificate = "/nonexistent/cert.pem";
	o.tls.private_key = "/nonexistent/key.pem";
	ASSERT_EQ(!server_check_tls(&o, err, sizeof(err)));
	ASSERT_EQ(err[0] != '\0');
}

#if WITH_CONFIG
static bool
load_text(const char* text, config_t* config, char* err, u32 err_size)
{
	char		path[]  = "/tmp/mogbus-test-XXXXXX";
	arena_t*	persist = ARENA_ALLOC();	/* kept: config points into it */
	arena_t*	scratch = ARENA_ALLOC();
	int		fd      = mkstemp(path);
	bool		ok;

	if (fd < 0 || write(fd, text, strlen(text)) < 0)
		return false;
	close(fd);
	ok = config_load(persist, scratch, path, config, err, err_size);
	unlink(path);
	arena_release(scratch);
	return ok;
}

TEST(ServerOptionsFromConfigMapsEachTransport)
{
	config_t	c = { 0 };
	char		err[256] = "";
	server_options_t o;

	ASSERT_EQ(load_text("(server (mode tcp) (bind \"0.0.0.0\") (port 1777) (seed 9) "
			    "(idle-timeout-ms 500)) (device (unit-id 4) (holding 0))",
			    &c, err, sizeof(err)));
	o = server_options_from_config(&c);
	ASSERT_EQ(o.mode == SERVER_MODE_TCP && o.port == 1777 && o.seed == 9);
	ASSERT_EQ(strcmp(o.bind_address, "0.0.0.0") == 0 && o.idle_timeout_ms == 500);
	ASSERT_EQ(!o.tls_enabled && o.fd == -1);
	ASSERT_EQ(o.device_count == 1 && o.devices == c.devices && o.devices[0]->unit_id == 4);

	memset(&c, 0, sizeof(c));
	ASSERT_EQ(load_text("(server (mode rtu-tcp) (port 1888) (baud 19200) (frame-gap-ms 7) "
			    "(broadcast-execute on)) (device (unit-id 1) (holding 0))",
			    &c, err, sizeof(err)));
	o = server_options_from_config(&c);
	ASSERT_EQ(o.mode == SERVER_MODE_RTU_TCP && o.port == 1888 && o.baud == 19200);
	ASSERT_EQ(o.frame_gap_ms == 7 && o.broadcast_execute);

	memset(&c, 0, sizeof(c));
	ASSERT_EQ(load_text("(server (mode rtu-device) (path \"/dev/ttyS9\") (baud 9600) "
			    "(parity even) (stop-bits 2) (strict-t15 on)) "
			    "(device (unit-id 1) (holding 0))", &c, err, sizeof(err)));
	o = server_options_from_config(&c);
	ASSERT_EQ(o.mode == SERVER_MODE_RTU_DEVICE && strcmp(o.path, "/dev/ttyS9") == 0);
	ASSERT_EQ(o.baud == 9600 && o.parity == PARITY_EVEN && o.stop_bits == 2 && o.strict_t15);

	memset(&c, 0, sizeof(c));
	ASSERT_EQ(load_text("(server (mode ascii-device) (path \"/dev/ttyS8\") (data-bits 7) "
			    "(parity even)) (device (unit-id 1) (holding 0))", &c, err, sizeof(err)));
	o = server_options_from_config(&c);
	ASSERT_EQ(o.mode == SERVER_MODE_ASCII_DEVICE && o.data_bits == 7 && o.parity == PARITY_EVEN);

	memset(&c, 0, sizeof(c));
	ASSERT_EQ(load_text("(server (mode ascii-tcp) (port 1999)) (device (unit-id 1) (holding 0))",
			    &c, err, sizeof(err)));
	o = server_options_from_config(&c);
	ASSERT_EQ(o.mode == SERVER_MODE_ASCII_TCP && o.port == 1999);
}
#endif
