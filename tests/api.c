/* Public C API (include/mogbus.h), driven in-process the way an embedding host would. */
#define _GNU_SOURCE
#include "vtest.h"
#include "build_config.h"
#include "mogbus.h"
#include "modbus/ascii.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "os/os_time.h"
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

TEST(ApiReadOnlyPointsRefuseClientsButNotTheHost)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev = mogbus_add_device(ctx, 1, "ro", NULL, 0);
	mogbus_point_t	p = holding(0, 5);
	uint16_t	words[1] = { 77 };
	double		v = 0;
	char		err[128];

	p.read_only = true;
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_INPUT, .address = 0, .type = MOGBUS_TYPE_U16, .read_only = true };
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));

	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 0, 1, words));
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) && v == 77);
	ASSERT_EQ(mogbus_device_set_value(dev, MOGBUS_HOLDING, 0, 12));
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) && v == 12);
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
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL);	/* no devices yet */
	ASSERT_EQ(strstr(err, "no devices") != NULL);

	dev = mogbus_add_device(ctx, 1, "tcp", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, err, sizeof(err)));
	server = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
	ASSERT_EQ(server != NULL);
	ASSERT_EQ(mogbus_server_pty_name(server) == NULL);
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && err[0]);	/* port in use */

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
	other = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
	ASSERT_EQ(other != NULL);
	o.mode = MOGBUS_MODE_RTU_DEVICE;
	o.path = NULL;
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && strstr(err, "path"));
	o.mode = MOGBUS_MODE_ASCII_FD;
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && strstr(err, "fd"));
	o = mogbus_server_options_defaults();
	o.tls_enabled = true;	/* unreadable files, or a build without TLS */
	o.tls_certificate = "/nonexistent/cert.pem";
	o.tls_private_key = "/nonexistent/key.pem";
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && err[0]);
	o = mogbus_server_options_defaults();
	o.bind_address = "01234567890123456789012345678901234567890123456789012345678901234567890123456789"
			 "01234567890123456789012345678901234567890123456789";
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && strstr(err, "too long"));
	mogbus_destroy(ctx);	/* closes `other` */
}

/* Runs mogbus_probe() in a child (it blocks) while this process steps the server. */
static bool
probe_child(mogbus_server_t* server, const mogbus_server_options_t* o, uint8_t unit)
{
	char	err[128];
	u64	deadline = os_now_ms() + 5000;
	int	status   = 0;
	pid_t	pid      = fork();

	if (pid == 0)
		_exit(mogbus_probe(o, unit, 2000, err, sizeof(err)) ? 0 : 1);
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
		mogbus_t*	ctx = mogbus_create();
		mogbus_server_options_t o = mogbus_server_options_defaults();
		mogbus_server_t* server;
		char		err[128];

		ASSERT_EQ(ctx != NULL && mogbus_add_device(ctx, 9, "probe", err, sizeof(err)) != NULL);
		o.mode = modes[i];
		o.port = ts_free_port();
		server = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
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
	char err[128];

	o.port = ts_free_port();
	ASSERT_EQ(!mogbus_probe(&o, 1, 500, err, sizeof(err)) && strstr(err, "connect") != NULL);
	o.mode = MOGBUS_MODE_UDP;
	ASSERT_EQ(!mogbus_probe(&o, 1, 500, err, sizeof(err)) && err[0]);
	o.mode = MOGBUS_MODE_RTU_PTY;
	ASSERT_EQ(!mogbus_probe(&o, 1, 500, err, sizeof(err)) && strstr(err, "network mode") != NULL);
	o.mode = MOGBUS_MODE_TCP;
	ASSERT_EQ(!mogbus_probe(&o, 0, 500, err, sizeof(err)) && strstr(err, "broadcast") != NULL);
	ASSERT_EQ(!mogbus_probe(NULL, 1, 500, err, sizeof(err)));
}

TEST(ApiProbeFailsWhenTheUnitIsUnknownOrSilent)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;
	char		err[128];

	ASSERT_EQ(ctx != NULL && mogbus_add_device(ctx, 9, "probe", err, sizeof(err)) != NULL);
	o.mode = MOGBUS_MODE_RTU_TCP;	/* an unknown RTU unit gets no reply */
	o.port = ts_free_port();
	server = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
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
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 0x1234);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_options_t clash;
	mogbus_server_t* server;
	mogbus_error_t	status = MOGBUS_OK;
	char		err[256];
	static char	text[40000];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	int		fd;

	ASSERT_EQ(ctx != NULL);
	dev = mogbus_add_device(ctx, 1, "http", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, err, sizeof(err)));
	ASSERT_EQ(o.http_bind_address == NULL && o.http_port == 9102);
	o.port              = ts_free_port();
	o.http_bind_address = "127.0.0.1";
	o.http_port         = ts_free_port();
	server = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
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
	ASSERT_EQ(mogbus_server_create(ctx, &clash, err, sizeof(err), &status) == NULL);
	ASSERT_EQ(status == MOGBUS_ERR_LISTEN);
	clash.http_bind_address = "localhost";
	ASSERT_EQ(mogbus_server_create(ctx, &clash, err, sizeof(err), &status) == NULL);
	ASSERT_EQ(status == MOGBUS_ERR_INVALID && strstr(err, "HTTP bind address") != NULL);

	mogbus_server_stop(server);
	ASSERT_EQ(mogbus_server_step(server, 0) == MOGBUS_STEP_DONE);
	mogbus_server_close(server);
	mogbus_destroy(ctx);
}

TEST(ApiServesDevicesOverUdp)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 0x1234);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;
	char		err[256];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	int		fd;

	ASSERT_EQ(ctx != NULL);
	dev = mogbus_add_device(ctx, 1, "udp", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, err, sizeof(err)));
	o.mode = MOGBUS_MODE_UDP;
	o.port = ts_free_port();
	server = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
	ASSERT_EQ(server != NULL);

	fd = ts_udp_loopback(o.port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0);
	ASSERT_EQ(ts_mbap_send(fd, 1, 7, READ_HOLDING, sizeof(READ_HOLDING)));
	ASSERT_EQ(pump(server, fd));
	ASSERT_EQ(ts_udp_mbap_recv(fd, 1, 7, resp, &len, 1000));
	ASSERT_EQ(len == 4 && resp[2] == 0x12 && resp[3] == 0x34);
	close(fd);

	mogbus_server_stop(server);
	ASSERT_EQ(mogbus_server_step(server, 0) == MOGBUS_STEP_DONE);
	mogbus_server_close(server);

	o.mode = (mogbus_mode_t)(MOGBUS_MODE_RTU_UDP + 1);
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && err[0]);
	mogbus_destroy(ctx);
}

TEST(ApiServesDevicesOverAsciiUdp)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 0x1234);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;
	char		err[256];
	u8		raw[MODBUS_ASCII_MAX_FRAME];
	u8		decoded[MODBUS_MAX_PDU + 2];
	u8		addr = 0;
	const u8*	pdu = NULL;
	u32		pdu_len = 0;
	s32		got;
	int		fd;

	ASSERT_EQ(ctx != NULL);
	dev = mogbus_add_device(ctx, 1, "ascii-udp", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, err, sizeof(err)));
	o.mode = MOGBUS_MODE_ASCII_UDP;
	o.port = ts_free_port();
	server = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
	ASSERT_EQ(server != NULL);

	fd = ts_udp_loopback(o.port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0);
	got = (s32)modbus_ascii_encode(raw, 1, READ_HOLDING, sizeof(READ_HOLDING));
	ASSERT_EQ(got > 0 && send(fd, raw, (size_t)got, 0) == got);
	ASSERT_EQ(pump(server, fd));
	got = ts_udp_recv(fd, raw, sizeof(raw), 1000);
	ASSERT_EQ(got > 0 && modbus_ascii_decode(raw, (u32)got, decoded, &addr, &pdu, &pdu_len));
	ASSERT_EQ(addr == 1 && pdu_len == 4 && pdu[2] == 0x12 && pdu[3] == 0x34);
	close(fd);

	mogbus_server_stop(server);
	ASSERT_EQ(mogbus_server_step(server, 0) == MOGBUS_STEP_DONE);
	mogbus_server_close(server);

	o.mode = (mogbus_mode_t)(MOGBUS_MODE_RTU_UDP + 1);
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && err[0]);
	mogbus_destroy(ctx);
}

TEST(ApiServesDevicesOverRtuUdp)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 0x1234);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;
	char		err[256];
	u8		raw[MODBUS_RTU_MAX_FRAME];
	const u8*	pdu = NULL;
	u8		addr = 0;
	u32		len = 0;
	s32		got;
	int		fd;

	ASSERT_EQ(ctx != NULL);
	dev = mogbus_add_device(ctx, 1, "rtu-udp", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, err, sizeof(err)));
	o.mode = MOGBUS_MODE_RTU_UDP;
	o.port = ts_free_port();
	server = mogbus_server_create(ctx, &o, err, sizeof(err), NULL);
	ASSERT_EQ(server != NULL);

	fd = ts_udp_loopback(o.port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0);
	len = modbus_rtu_encode(raw, 1, READ_HOLDING, sizeof(READ_HOLDING));
	ASSERT_EQ(send(fd, raw, len, 0) == (ssize_t)len);
	ASSERT_EQ(pump(server, fd));
	got = ts_udp_recv(fd, raw, sizeof(raw), 1000);
	ASSERT_EQ(got > 0 && modbus_rtu_decode(raw, (u32)got, &addr, &pdu, &len));
	ASSERT_EQ(addr == 1 && len == 4 && pdu[2] == 0x12 && pdu[3] == 0x34);
	close(fd);

	mogbus_server_stop(server);
	ASSERT_EQ(mogbus_server_step(server, 0) == MOGBUS_STEP_DONE);
	mogbus_server_close(server);

	o.mode = (mogbus_mode_t)99;
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), NULL) == NULL && err[0]);
	mogbus_destroy(ctx);
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

	ASSERT_EQ(mogbus_server_create_from_config(ctx, err, sizeof(err), NULL) == NULL);	/* nothing loaded */
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
	server = mogbus_server_create_from_config(ctx, err, sizeof(err), NULL);
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
	ASSERT_EQ(mogbus_server_create_from_config(ctx, err, sizeof(err), NULL) == NULL);
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
		server = mogbus_server_create(ctx, &o, NULL, 0, NULL);
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
	server = mogbus_server_create(ctx, &o, NULL, 0, NULL);
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

TEST(ApiServerCheckValidatesWithoutOpeningAnything)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p = holding(0, 1);
	mogbus_server_options_t o = mogbus_server_options_defaults();
	mogbus_server_t* server;
	mogbus_error_t	status = MOGBUS_OK;
	char		err[256] = "";

	ASSERT_EQ(ctx != NULL);
	ASSERT_EQ(!mogbus_server_check(ctx, &o, err, sizeof(err)) && strstr(err, "no devices"));
	ASSERT_EQ(mogbus_server_create(ctx, &o, NULL, 0, &status) == NULL && status == MOGBUS_ERR_INVALID);
	dev = mogbus_add_device(ctx, 1, "chk", err, sizeof(err));
	ASSERT_EQ(dev != NULL && mogbus_device_add_point(dev, &p, err, sizeof(err)));

	/* A port that is already in use passes the check, but not the create. */
	o.port = ts_free_port();
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
	server = mogbus_server_create(ctx, &o, err, sizeof(err), &status);
	ASSERT_EQ(server != NULL && status == MOGBUS_OK);
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), &status) == NULL &&
		  status == MOGBUS_ERR_LISTEN);

	/* A missing serial device passes the check (nothing is opened) and fails on create. */
	o = mogbus_server_options_defaults();
	o.mode = MOGBUS_MODE_RTU_DEVICE;
	o.path = "/nonexistent/ttyMOGBUS";
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), &status) == NULL &&
		  status == MOGBUS_ERR_SERIAL && strstr(err, "serial") != NULL);
	o.path = NULL;
	ASSERT_EQ(!mogbus_server_check(ctx, &o, err, sizeof(err)) && strstr(err, "path"));
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), &status) == NULL &&
		  status == MOGBUS_ERR_INVALID);
	o = mogbus_server_options_defaults();
	o.stop_bits = 3;
	ASSERT_EQ(!mogbus_server_check(ctx, &o, err, sizeof(err)) && strstr(err, "serial line"));
	o = mogbus_server_options_defaults();
	o.bind_address = "01234567890123456789012345678901234567890123456789012345678901234567890123456789"
			 "01234567890123456789012345678901234567890123456789";
	ASSERT_EQ(!mogbus_server_check(ctx, &o, err, sizeof(err)) && strstr(err, "too long"));
	o = mogbus_server_options_defaults();
	o.mode = (mogbus_mode_t)99;
	ASSERT_EQ(!mogbus_server_check(ctx, &o, err, sizeof(err)) && err[0]);

	/* TLS files are loaded for the check (unreadable here, or a build without TLS). */
	o = mogbus_server_options_defaults();
	o.tls_enabled = true;
	o.tls_certificate = "/nonexistent/cert.pem";
	o.tls_private_key = "/nonexistent/key.pem";
	ASSERT_EQ(!mogbus_server_check(ctx, &o, err, sizeof(err)) && err[0]);
	ASSERT_EQ(!mogbus_server_check(ctx, &o, NULL, 0));
	ASSERT_EQ(mogbus_server_create(ctx, &o, err, sizeof(err), &status) == NULL &&
		  status == MOGBUS_ERR_LISTEN);
#if WITH_TLS
	o.tls_certificate = "tests/tls/server.pem";
	o.tls_private_key = "tests/tls/server.key";
	o.tls_ca_certificate = "tests/tls/ca.pem";
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
	o.tls_ca_certificate = NULL;	/* client certificates are required by default */
	ASSERT_EQ(!mogbus_server_check(ctx, &o, err, sizeof(err)) && strstr(err, "CA"));
	o.tls_require_client_certificate = false;
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
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
	mogbus_t*	ctx = mogbus_create();
	mogbus_server_options_t o;
	char		err[256] = "";

	ASSERT_EQ(ctx != NULL && !mogbus_config_server_options(ctx, &o));	/* nothing loaded */
	ASSERT_EQ(write_config(path,
		"(server (mode rtu-device) (path \"/dev/ttyUSB9\") (baud 19200)\n"
		"        (parity even) (stop-bits 2) (strict-t15 on) (broadcast-execute on)\n"
		"        (seed 42))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path, err, sizeof(err)));
	ASSERT_EQ(mogbus_config_server_options(ctx, &o));
	ASSERT_EQ(o.mode == MOGBUS_MODE_RTU_DEVICE && strcmp(o.path, "/dev/ttyUSB9") == 0);
	ASSERT_EQ(o.baud == 19200 && o.frame_gap_ms == 0 && o.seed == 42);
	ASSERT_EQ(o.data_bits == 8 && o.parity == MOGBUS_PARITY_EVEN && o.stop_bits == 2);
	ASSERT_EQ(o.strict_t15 && o.broadcast_execute && !o.tls_enabled && o.fd == -1);
	mogbus_destroy(ctx);
	unlink(path);

	ctx = mogbus_create();
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode ascii-device) (path \"/dev/ttyUSB8\") (data-bits 7) (parity even))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path, err, sizeof(err)));
	ASSERT_EQ(mogbus_config_server_options(ctx, &o));
	ASSERT_EQ(o.mode == MOGBUS_MODE_ASCII_DEVICE && o.data_bits == 7 && o.stop_bits == 1);
	mogbus_destroy(ctx);
	unlink(path);

	ctx = mogbus_create();
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode ascii-tcp) (port 4321) (bind \"0.0.0.0\") (idle-timeout-ms 1500))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path, err, sizeof(err)));
	ASSERT_EQ(mogbus_config_server_options(ctx, &o));
	ASSERT_EQ(o.mode == MOGBUS_MODE_ASCII_TCP && o.port == 4321 && o.idle_timeout_ms == 1500);
	ASSERT_EQ(strcmp(o.bind_address, "0.0.0.0") == 0 && o.path == NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
	mogbus_destroy(ctx);
	unlink(path);

#if WITH_TLS
	ctx = mogbus_create();
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode tcp) (port 4802) (tls (certificate \"tests/tls/server.pem\")\n"
		"  (private-key \"tests/tls/server.key\") (ca-certificate \"tests/tls/ca.pem\")\n"
		"  (handshake-timeout-ms 777) (min-version tls1.3)))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path, err, sizeof(err)));
	ASSERT_EQ(mogbus_config_server_options(ctx, &o));
	ASSERT_EQ(o.mode == MOGBUS_MODE_TCP && o.port == 4802 && o.tls_enabled);
	ASSERT_EQ(strcmp(o.tls_certificate, "tests/tls/server.pem") == 0);
	ASSERT_EQ(strcmp(o.tls_private_key, "tests/tls/server.key") == 0);
	ASSERT_EQ(strcmp(o.tls_ca_certificate, "tests/tls/ca.pem") == 0);
	ASSERT_EQ(o.tls_handshake_timeout_ms == 777 && o.tls_require_client_certificate);
	ASSERT_EQ(o.tls_min_version == MOGBUS_TLS_1_3 && o.tls_ciphers_1_2 == NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
	mogbus_destroy(ctx);
	unlink(path);

	/* Without a CA and without required client certificates TLS still works. */
	ctx = mogbus_create();
	strcpy(path, "/tmp/mogbus-api-XXXXXX");
	ASSERT_EQ(write_config(path,
		"(server (mode tcp) (tls (certificate \"tests/tls/server.pem\")\n"
		"  (private-key \"tests/tls/server.key\") (require-client-certificate off)))\n"
		"(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path, err, sizeof(err)));
	ASSERT_EQ(mogbus_config_server_options(ctx, &o) && o.tls_ca_certificate == NULL);
	ASSERT_EQ(mogbus_server_check(ctx, &o, err, sizeof(err)));
	mogbus_destroy(ctx);
	unlink(path);
#endif
}

TEST(ApiEnumeratesDevicesAndDescribesThem)
{
	char		path[] = "/tmp/mogbus-api-XXXXXX";
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_info_t info;
	mogbus_point_t	p = holding(0, 1);
	char		err[256] = "";

	ASSERT_EQ(mogbus_device_count(ctx) == 0 && mogbus_device_at(ctx, 0) == NULL);
	ASSERT_EQ(write_config(path,
		"(device (name \"meter\") (unit-id 3)\n"
		"  (coil 0 (static 1)) (coil 1 (static 0)) (discrete 5 (static 1))\n"
		"  (input 0 (static 1)) (holding 0 (static 2)) (holding 1 (static 3)) (holding 2 (static 4))\n"
		"  (fault (kind delay) (percent 25) (delay-ms 40))\n"
		"  (fault (kind exception) (code 6) (percent 10))\n"
		"  (fault (kind drop) (schedule (step (after-ms 100) (count 2))))\n"
		"  (fault (kind sensor-disconnect) (percent 100)))\n"
		"(device (unit-id 9) (holding 0))\n"));
	ASSERT_EQ(mogbus_load_config(ctx, path, err, sizeof(err)));
	{
		mogbus_device_t* extra = mogbus_add_device(ctx, 11, "code", err, sizeof(err));

		ASSERT_EQ(extra != NULL && mogbus_device_add_point(extra, &p, err, sizeof(err)));
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
	mogbus_destroy(ctx);
	unlink(path);
}
#else
TEST(ApiConfigOptionsAreUnavailableWithoutConfigSupport)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_server_options_t o;

	ASSERT_EQ(ctx != NULL && !mogbus_config_server_options(ctx, &o));
	mogbus_destroy(ctx);
}
#endif

TEST(ApiSixtyFourBitByteOrderAndStringPoints)
{
	mogbus_t*	ctx = mogbus_create();
	mogbus_device_t* dev;
	mogbus_point_t	p;
	uint16_t	w[4] = { 0 };
	double		v = 0;
	char		err[128];
	char		text[16];

	ASSERT_EQ(ctx != NULL);
	dev = mogbus_add_device(ctx, 1, NULL, err, sizeof(err));
	ASSERT_EQ(dev != NULL);

	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_U64,
			      .value = 4294967296.0, .byte_order = MOGBUS_BYTES_LITTLE };
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 0, 4, w));
	ASSERT_EQ(w[0] == 0 && w[1] == 0x0100 && w[2] == 0 && w[3] == 0);
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &v) && v == 4294967296.0);
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 4, .type = MOGBUS_TYPE_F64,
			      .value = -2.5, .word_order = MOGBUS_WORDS_LOW_FIRST };
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	ASSERT_EQ(mogbus_device_read(dev, MOGBUS_HOLDING, 4, 4, w));
	ASSERT_EQ(w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0xC004);
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 8, .type = MOGBUS_TYPE_S64, .value = -1 };
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	ASSERT_EQ(mogbus_device_get_value(dev, MOGBUS_HOLDING, 8, &v) && v == -1);

	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 20, .type = MOGBUS_TYPE_STRING,
			      .length = 4, .text = "SN-1234" };
	ASSERT_EQ(mogbus_device_add_point(dev, &p, err, sizeof(err)));
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, sizeof(text)));
	ASSERT_EQ(strcmp(text, "SN-1234") == 0);
	ASSERT_EQ(!mogbus_device_get_value(dev, MOGBUS_HOLDING, 20, &v));	/* a string has no number */
	ASSERT_EQ(!mogbus_device_set_value(dev, MOGBUS_HOLDING, 20, 1));
	ASSERT_EQ(!mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, 4));	/* too small */
	ASSERT_EQ(!mogbus_device_get_string(dev, MOGBUS_HOLDING, 0, text, sizeof(text)));	/* not a string */
	ASSERT_EQ(mogbus_device_set_string(dev, MOGBUS_HOLDING, 20, "NEW"));
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, sizeof(text)));
	ASSERT_EQ(strcmp(text, "NEW") == 0);
	ASSERT_EQ(!mogbus_device_set_string(dev, MOGBUS_HOLDING, 20, "123456789"));	/* > 2*length */
	ASSERT_EQ(!mogbus_device_set_string(dev, MOGBUS_HOLDING, 21, "A"));
	/* A client write still works and shows up in get_string. */
	w[0] = 0x4F4B;
	w[1] = w[2] = w[3] = 0;
	ASSERT_EQ(mogbus_device_write(dev, MOGBUS_HOLDING, 20, 4, w));
	ASSERT_EQ(mogbus_device_get_string(dev, MOGBUS_HOLDING, 20, text, sizeof(text)));
	ASSERT_EQ(strcmp(text, "OK") == 0);

	/* strings are limited to 1..123 registers and need a register table */
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 100, .type = MOGBUS_TYPE_STRING };
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)) && strstr(err, "between 1 and 123"));
	p.length = 124;
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 100, .type = MOGBUS_TYPE_STRING,
			      .length = 1, .text = "TOO LONG" };
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_COIL, .address = 0, .type = MOGBUS_TYPE_STRING, .length = 1 };
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));
	p = (mogbus_point_t){ .space = MOGBUS_HOLDING, .address = 100, .type = MOGBUS_TYPE_U16,
			      .byte_order = (mogbus_byte_order_t)7 };
	ASSERT_EQ(!mogbus_device_add_point(dev, &p, err, sizeof(err)));
	mogbus_destroy(ctx);
}
