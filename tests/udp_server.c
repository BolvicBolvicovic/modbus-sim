#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Integration tests for Modbus UDP. Each test starts the real binary on a loopback
 * port; the fixture cleans up on failure (PRE_TEST/atexit). The port comes from
 * ts_free_port (a TCP probe; UDP ports are a separate space, so a clash is possible
 * but rare). Readiness comes from the stderr "listening on UDP" line. */

#define RECV_TIMEOUT_MS	1000
#define QUIET_MS	200

static ts_server_t g_fx = {
	.pid		= -1,
	.master_fd	= -1,
	.flags		= TS_CAPTURE_STDERR | TS_DEATH_SIGNAL | TS_CLEAN_EXIT,
	.label		= "udp test",
	.ready_needle	= "listening on UDP"
};

static const u8 READ_HOLDING_0[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };

static void
server_start(const char* server_options, const char* devices)
{
	const char* bin = getenv("MOGBUS_BIN");
	char	    config[2048];
	char*	    argv[3];
	int	    n;

	g_fx.probe_port = ts_free_port();
	ASSERT(g_fx.probe_port != 0);
	n = snprintf(config, sizeof(config), "(server (mode udp) (port %u) (bind \"127.0.0.1\") %s)\n%s\n",
		     g_fx.probe_port, server_options, devices);
	ASSERT(n > 0 && (size_t)n < sizeof(config));
	ASSERT_EQ(ts_write_config(&g_fx, "/tmp/mogbus-udp-XXXXXX", config));

	argv[0] = (char*)(bin ? bin : "./mogbus");
	argv[1] = g_fx.config;
	argv[2] = NULL;
	ASSERT_EQ(ts_server_start(&g_fx, argv));
}

static void
server_stop(void)
{
	ASSERT_EQ(ts_server_stop(&g_fx));
	g_fx.probe_port = 0;
}

static void
server_cleanup(void)
{
	ts_server_cleanup(&g_fx);
}

static int
client_open(void)
{
	return ts_udp_loopback(g_fx.probe_port, AF_INET, RECV_TIMEOUT_MS);
}

/* One request/response; false when no valid reply arrives in time. */
static bool
exchange(int fd, u8 unit, const u8* req, u32 req_len, u8* resp, u32* resp_len)
{
	u16 tid = ts_next_tid();

	return ts_mbap_send(fd, unit, tid, req, req_len) &&
	       ts_udp_mbap_recv(fd, unit, tid, resp, resp_len, RECV_TIMEOUT_MS);
}

/* True when nothing arrives within QUIET_MS. */
static bool
silent(int fd)
{
	u8 buf[MBAP_MAX_FRAME];

	return ts_udp_recv(fd, buf, sizeof(buf), QUIET_MS) < 0;
}

SETUP_SUITE()
{
	(void)_ss_arena;
	atexit(server_cleanup);
}

PRE_TEST()
{
	server_stop();
}

TEST(UdpServesReadHoldingRegisters)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 4660)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && len == 4 && get_be16(resp + 2) == 4660);
	close(fd);
	server_stop();
}

TEST(UdpWriteThenRead)
{
	u8  write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x04, 0xD2 };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(exchange(fd, 1, write_pdu, sizeof(write_pdu), resp, &len));
	ASSERT_EQ(len == 5 && resp[0] == FC_WRITE_SINGLE_REGISTER);
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 1234);
	close(fd);
	server_stop();
}

TEST(UdpRoutesByUnitAndUnknownUnitGetsGatewayException)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 11)))\n"
			 "(device (unit-id 2) (holding 0 (static 22)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(exchange(fd, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 22);
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 11);
	ASSERT_EQ(exchange(fd, 9, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(len == 2 && resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80) &&
		  resp[1] == EXC_GATEWAY_TARGET_FAILED);
	close(fd);
	server_stop();
}

TEST(UdpIgnoresMalformedDatagramsAndKeepsServing)
{
	u8  frame[MBAP_MAX_FRAME + 40];
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0, n;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 7)))\n");
	fd = client_open();
	ASSERT(fd >= 0);

	/* Bad protocol id. */
	n = ts_mbap_encode(frame, 1, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0));
	frame[3] = 1;
	ASSERT_EQ(send(fd, frame, n, 0) == (ssize_t)n);
	ASSERT_EQ(silent(fd));

	/* Length field larger than the datagram, and smaller than it. */
	n = ts_mbap_encode(frame, 1, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0));
	ASSERT_EQ(send(fd, frame, n - 1, 0) == (ssize_t)(n - 1));
	ASSERT_EQ(silent(fd));
	frame[n] = 0;
	ASSERT_EQ(send(fd, frame, n + 1, 0) == (ssize_t)(n + 1));
	ASSERT_EQ(silent(fd));

	/* Shorter than a header, header only, and an empty datagram. */
	ASSERT_EQ(send(fd, frame, 3, 0) == 3);
	ASSERT_EQ(send(fd, frame, MBAP_HEADER_SIZE, 0) == MBAP_HEADER_SIZE);
	ASSERT_EQ(send(fd, frame, 0, 0) == 0);
	ASSERT_EQ(silent(fd));

	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 7);
	close(fd);
	server_stop();
}

TEST(UdpDropsOversizedDatagram)
{
	u8  frame[MBAP_MAX_FRAME + 100] = { 0 };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 7)))\n");
	fd = client_open();
	ASSERT(fd >= 0);

	/* A valid ADU followed by trailing bytes that overflow the receive buffer, so the
	 * kernel truncates it on read; it must be dropped, not answered from the prefix. */
	(void)ts_mbap_encode(frame, 1, 3, READ_HOLDING_0, sizeof(READ_HOLDING_0));
	ASSERT_EQ(send(fd, frame, 300, 0) == 300);
	ASSERT_EQ(silent(fd));

	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 7);
	close(fd);
	server_stop();
}

TEST(UdpDelayFaultDoesNotBlockOtherPeers)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	u64 start, fast_ms, delayed_ms;
	u16 tid_slow = ts_next_tid();
	int slow, fast;

	server_start("", "(device (unit-id 1) (holding 0 (static 11))\n"
			 "  (fault (kind delay) (delay-ms 500) (percent 100)))\n"
			 "(device (unit-id 2) (holding 0 (static 22)))\n");
	slow = client_open();
	fast = client_open();
	ASSERT(slow >= 0 && fast >= 0);

	start = os_now_ms();
	ASSERT_EQ(ts_mbap_send(slow, 1, tid_slow, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(exchange(fast, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	fast_ms = os_now_ms() - start;
	ASSERT_EQ(get_be16(resp + 2) == 22);

	ASSERT_EQ(ts_udp_mbap_recv(slow, 1, tid_slow, resp, &len, 2000));
	delayed_ms = os_now_ms() - start;
	ASSERT_EQ(get_be16(resp + 2) == 11);
	ASSERT_EQ(fast_ms < 300);
	ASSERT_EQ(delayed_ms >= 400);
	close(slow);
	close(fast);
	server_stop();
}

TEST(UdpDropFaultStillAppliesWrites)
{
	u8  write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x04, 0xD2 };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	/* Device 1 drops every response; device 2 shares nothing but proves liveness. */
	server_start("", "(device (unit-id 1) (holding 0 (static 0))\n"
			 "  (fault (kind drop) (percent 100)))\n"
			 "(device (unit-id 2) (holding 0 (static 5)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(ts_mbap_send(fd, 1, ts_next_tid(), write_pdu, sizeof(write_pdu)));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(exchange(fd, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 5);
	close(fd);
	server_stop();
}

TEST(UdpRepliesToEachPeerSeparately)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	u16 tid_a = ts_next_tid();
	u16 tid_b = ts_next_tid();
	int a, b;

	server_start("", "(device (unit-id 1) (holding 0 (static 33)))\n");
	a = client_open();
	b = client_open();
	ASSERT(a >= 0 && b >= 0);

	/* Interleaved requests from two source ports each get their own reply. */
	ASSERT_EQ(ts_mbap_send(a, 1, tid_a, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(ts_mbap_send(b, 1, tid_b, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(ts_udp_mbap_recv(b, 1, tid_b, resp, &len, RECV_TIMEOUT_MS));
	ASSERT_EQ(get_be16(resp + 2) == 33);
	ASSERT_EQ(ts_udp_mbap_recv(a, 1, tid_a, resp, &len, RECV_TIMEOUT_MS));
	ASSERT_EQ(get_be16(resp + 2) == 33);
	ASSERT_EQ(silent(a) && silent(b));
	close(a);
	close(b);
	server_stop();
}

TEST(UdpExceptionFaultReplacesResponse)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 1))\n"
			 "  (fault (kind exception) (code 6) (percent 100)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(len == 2 && resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80) && resp[1] == 6);
	close(fd);
	server_stop();
}

/* A second server on a port already bound must fail to start (exit 2), not share it. */
TEST(UdpPortInUseFailsToStart)
{
	const char* bin    = getenv("MOGBUS_BIN");
	ts_server_t second = { .pid = -1, .master_fd = -1, .label = "udp second",
			       .flags = TS_CAPTURE_STDERR | TS_NULL_STDIN | TS_EXIT_IS_READY };
	char*	    argv[3];
	int	    status = 0;
	bool	    ok;

	server_start("", "(device (unit-id 1) (holding 0 (static 1)))\n");
	argv[0] = (char*)(bin ? bin : "./mogbus");
	argv[1] = g_fx.config;
	argv[2] = NULL;
	ASSERT_EQ(ts_server_start(&second, argv));
	ok = ts_wait_pid(second.pid, &status, 5000);
	second.pid = -1;
	ts_server_cleanup(&second);
	ASSERT_EQ(ok && WIFEXITED(status) && WEXITSTATUS(status) == 2);
	server_stop();
}
