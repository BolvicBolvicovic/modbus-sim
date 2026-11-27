#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Integration tests for RTU-over-UDP (one RTU frame per datagram). Each test starts
 * the real binary on a loopback port; the fixture cleans up on failure
 * (PRE_TEST/atexit). Readiness comes from the stderr "listening on" line. */

#define RECV_TIMEOUT_MS	1000
#define QUIET_MS	200

static ts_server_t g_fx = {
	.pid		= -1,
	.master_fd	= -1,
	.flags		= TS_CAPTURE_STDERR | TS_DEATH_SIGNAL | TS_CLEAN_EXIT,
	.label		= "rtu-udp test",
	.ready_needle	= "RTU-over-UDP listening on"
};

static const u8 READ_HOLDING_0[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
static const u8 WRITE_HOLDING_0[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x04, 0xD2 };
static const u8 DIAG_COMM_ERRORS[] = { FC_DIAGNOSTICS, 0x00, DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT, 0x00, 0x00 };

static void
server_start(const char* server_options, const char* devices)
{
	const char* bin = getenv("MOGBUS_BIN");
	char	    config[2048];
	char*	    argv[3];
	int	    n;

	g_fx.probe_port = ts_free_port();
	ASSERT(g_fx.probe_port != 0);
	n = snprintf(config, sizeof(config), "(server (mode rtu-udp) (port %u) (bind \"127.0.0.1\") %s)\n%s\n",
		     g_fx.probe_port, server_options, devices);
	ASSERT(n > 0 && (size_t)n < sizeof(config));
	ASSERT_EQ(ts_write_config(&g_fx, "/tmp/mogbus-rtuudp-XXXXXX", config));

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

static bool
send_frame(int fd, u8 addr, const u8* pdu, u32 pdu_len)
{
	u8  frame[MODBUS_RTU_MAX_FRAME];
	u32 len = modbus_rtu_encode(frame, addr, pdu, pdu_len);

	return len > 0 && send(fd, frame, len, 0) == (ssize_t)len;
}

/* One reply datagram that must be a valid RTU frame. */
static bool
recv_frame(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8	  raw[MODBUS_RTU_MAX_FRAME + 16];
	const u8* body = NULL;
	s32	  got = ts_udp_recv(fd, raw, sizeof(raw), timeout_ms);

	if (got < 4 ||
	    !modbus_rtu_decode(raw, (u32)got, addr, &body, pdu_len))
		return false;
	memcpy(pdu, body, *pdu_len);
	return true;
}

static bool
exchange(int fd, u8 unit, const u8* req, u32 req_len, u8* resp, u32* resp_len)
{
	u8 addr = 0;

	return send_frame(fd, unit, req, req_len) &&
	       recv_frame(fd, &addr, resp, resp_len, RECV_TIMEOUT_MS) &&
	       addr == unit;
}

/* True when nothing arrives within QUIET_MS. */
static bool
silent(int fd)
{
	u8 buf[MODBUS_RTU_MAX_FRAME + 16];

	return ts_udp_recv(fd, buf, sizeof(buf), QUIET_MS) < 0;
}

/* The device's FC08 0x000C bus communication error count. */
static s32
comm_errors(int fd, u8 unit)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;

	if (!exchange(fd, unit, DIAG_COMM_ERRORS, sizeof(DIAG_COMM_ERRORS), resp, &len) || len != 5)
		return -1;
	return get_be16(resp + 3);
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

TEST(RtuUdpWriteThenRead)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(exchange(fd, 1, WRITE_HOLDING_0, sizeof(WRITE_HOLDING_0), resp, &len));
	ASSERT_EQ(len == 5 && resp[0] == FC_WRITE_SINGLE_REGISTER);
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 1234);
	close(fd);
	server_stop();
}

TEST(RtuUdpBadCrcAndShortDatagramsAreIgnoredAndBadCrcIsCounted)
{
	u8  frame[MODBUS_RTU_MAX_FRAME];
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0, n;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 7)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(comm_errors(fd, 1) == 0);

	n = modbus_rtu_encode(frame, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0));
	frame[n - 1] ^= 0xFF;
	ASSERT_EQ(send(fd, frame, n, 0) == (ssize_t)n);
	ASSERT_EQ(silent(fd));

	/* Too short to be a frame: ignored and not counted; empty datagram likewise. */
	ASSERT_EQ(send(fd, frame, 3, 0) == 3);
	ASSERT_EQ(send(fd, frame, 0, 0) == 0);
	ASSERT_EQ(silent(fd));

	ASSERT_EQ(comm_errors(fd, 1) == 1);
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 7);
	close(fd);
	server_stop();
}

TEST(RtuUdpIgnoresUnknownUnit)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 11)))\n"
			 "(device (unit-id 2) (holding 0 (static 22)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_frame(fd, 9, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(exchange(fd, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 22);
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 11);
	close(fd);
	server_stop();
}

TEST(RtuUdpBroadcastGetsNoResponseAndIsIgnoredByDefault)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_frame(fd, 0, WRITE_HOLDING_0, sizeof(WRITE_HOLDING_0)));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 0);
	close(fd);
	server_stop();
}

TEST(RtuUdpBroadcastExecuteWritesEveryDeviceWithoutResponse)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("(broadcast-execute on)", "(device (unit-id 1) (holding 0 (static 0)))\n"
					       "(device (unit-id 2) (holding 0 (static 0)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_frame(fd, 0, WRITE_HOLDING_0, sizeof(WRITE_HOLDING_0)));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 1234);
	ASSERT_EQ(exchange(fd, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 1234);
	close(fd);
	server_stop();
}

TEST(RtuUdpDropsOversizedDatagram)
{
	u8  frame[MODBUS_RTU_MAX_FRAME + 100] = { 0 };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0, n;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 7)))\n");
	fd = client_open();
	ASSERT(fd >= 0);

	/* A valid frame followed by trailing bytes that overflow the receive buffer, so the
	 * kernel truncates it on read; it must be dropped, not answered from the prefix. */
	n = modbus_rtu_encode(frame, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0));
	ASSERT_EQ(n > 0);
	ASSERT_EQ(send(fd, frame, 300, 0) == 300);
	ASSERT_EQ(silent(fd));

	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 7);
	close(fd);
	server_stop();
}

TEST(RtuUdpRejectsTwoFramesInOneDatagram)
{
	u8  frame[2 * MODBUS_RTU_MAX_FRAME];
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0, n;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = client_open();
	ASSERT(fd >= 0);

	/* Two valid write frames back to back are one invalid datagram: nothing is
	 * executed (the register stays 0) and no reply is sent. */
	n = modbus_rtu_encode(frame, 1, WRITE_HOLDING_0, sizeof(WRITE_HOLDING_0));
	n += modbus_rtu_encode(frame + n, 1, WRITE_HOLDING_0, sizeof(WRITE_HOLDING_0));
	ASSERT_EQ(send(fd, frame, n, 0) == (ssize_t)n);
	ASSERT_EQ(silent(fd));

	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 0);
	ASSERT_EQ(comm_errors(fd, 1) == 1);
	close(fd);
	server_stop();
}

TEST(RtuUdpDropFaultStillAppliesWrites)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0))\n"
			 "  (fault (kind drop) (percent 100)))\n"
			 "(device (unit-id 2) (holding 0 (static 5)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_frame(fd, 1, WRITE_HOLDING_0, sizeof(WRITE_HOLDING_0)));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(exchange(fd, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 5);
	close(fd);
	server_stop();
}

TEST(RtuUdpCorruptChecksumFaultProducesBadCrc)
{
	u8  raw[MODBUS_RTU_MAX_FRAME + 16];
	u8  addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;
	s32 got;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 1))\n"
			 "  (fault (kind corrupt) (mode checksum) (percent 100)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_frame(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	got = ts_udp_recv(fd, raw, sizeof(raw), RECV_TIMEOUT_MS);
	ASSERT_EQ(got == 7);	/* addr + 1 fc + 1 byte count + 2 data + CRC */
	ASSERT_EQ(!modbus_rtu_decode(raw, (u32)got, &addr, &pdu, &pdu_len));
	raw[got - 1] ^= 0xFF;	/* undoing the flip restores a valid frame */
	ASSERT_EQ(modbus_rtu_decode(raw, (u32)got, &addr, &pdu, &pdu_len));
	close(fd);
	server_stop();
}

/* A second server on a port already bound must fail to start (exit 2), not share it. */
TEST(RtuUdpPortInUseFailsToStart)
{
	const char* bin    = getenv("MOGBUS_BIN");
	ts_server_t second = { .pid = -1, .master_fd = -1, .label = "rtu-udp second",
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
