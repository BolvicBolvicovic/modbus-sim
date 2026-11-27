#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Integration tests for Modbus ASCII over UDP. Each test starts the real binary on
 * a loopback port; the fixture cleans up on failure (PRE_TEST/atexit). The port comes
 * from ts_free_port (a TCP probe; UDP ports are a separate space, so a clash is
 * possible but rare). Readiness comes from the stderr "listening on ASCII-over-UDP" line. */

#define RECV_TIMEOUT_MS	1000
#define QUIET_MS	200

static ts_server_t g_fx = {
	.pid		= -1,
	.master_fd	= -1,
	.flags		= TS_CAPTURE_STDERR | TS_DEATH_SIGNAL | TS_CLEAN_EXIT,
	.label		= "ascii-udp test",
	.ready_needle	= "listening on ASCII-over-UDP"
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
	n = snprintf(config, sizeof(config), "(server (mode ascii-udp) (port %u) (bind \"127.0.0.1\") %s)\n%s\n",
		     g_fx.probe_port, server_options, devices);
	ASSERT(n > 0 && (size_t)n < sizeof(config));
	ASSERT_EQ(ts_write_config(&g_fx, "/tmp/mogbus-ascii-udp-XXXXXX", config));

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
send_raw(int fd, const char* text)
{
	size_t n = strlen(text);

	return send(fd, text, n, 0) == (ssize_t)n;
}

static bool
send_request(int fd, u8 unit, const u8* pdu, u32 pdu_len)
{
	u8  frame[MODBUS_ASCII_MAX_FRAME];
	u32 n = modbus_ascii_encode(frame, unit, pdu, pdu_len);

	return n > 0 && send(fd, frame, n, 0) == (ssize_t)n;
}

/* One reply datagram must be exactly one complete frame. */
static bool
recv_response(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8	  raw[MODBUS_ASCII_MAX_FRAME];
	u8	  decoded[MODBUS_MAX_PDU + 2];
	const u8* p        = NULL;
	u32	  scan_len = 0;
	s32	  got      = ts_udp_recv(fd, raw, sizeof(raw), timeout_ms);

	if (got <= 0 ||
	    modbus_ascii_scan(raw, (u32)got, &scan_len) != MODBUS_ASCII_SCAN_FRAME ||
	    scan_len != (u32)got ||
	    !modbus_ascii_decode(raw, (u32)got, decoded, addr, &p, pdu_len))
		return false;
	memcpy(pdu, p, *pdu_len);
	return true;
}

static bool
exchange(int fd, u8 unit, const u8* req, u32 req_len, u8* resp, u32* resp_len)
{
	u8 addr = 0;

	return send_request(fd, unit, req, req_len) &&
	       recv_response(fd, &addr, resp, resp_len, RECV_TIMEOUT_MS) &&
	       addr == unit;
}

/* True when nothing arrives within QUIET_MS. */
static bool
silent(int fd)
{
	u8 buf[MODBUS_ASCII_MAX_FRAME];

	return ts_udp_recv(fd, buf, sizeof(buf), QUIET_MS) < 0;
}

/* FC08 0x000C counter of `unit`, or 0xFFFF when the query fails. */
static u16
comm_errors(int fd, u8 unit)
{
	u8  diag[] = { FC_DIAGNOSTICS, 0x00, DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT, 0x00, 0x00 };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;

	if (!exchange(fd, unit, diag, sizeof(diag), resp, &len) || len != 5)
		return 0xFFFF;
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

TEST(AsciiUdpWriteThenRead)
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

TEST(AsciiUdpRoutesByUnit)
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
	close(fd);
	server_stop();
}

TEST(AsciiUdpAcceptsLowercaseRequestAndRepliesUppercase)
{
	u8  raw[MODBUS_ASCII_MAX_FRAME];
	s32 got;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 43981)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_raw(fd, ":010300000001fb\r\n"));
	got = ts_udp_recv(fd, raw, sizeof(raw), RECV_TIMEOUT_MS);
	ASSERT_EQ(got == 15 && memcmp(raw, ":010302ABCD", 11) == 0);
	for (s32 i = 0; i < got; i++)
		ASSERT_EQ(!(raw[i] >= 'a' && raw[i] <= 'f'));
	ASSERT_EQ(raw[got - 2] == '\r' && raw[got - 1] == '\n');
	close(fd);
	server_stop();
}

TEST(AsciiUdpBadLrcIsSilentAndCountsForEveryDevice)
{
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 1)))\n"
			 "(device (unit-id 2) (holding 0 (static 2)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_raw(fd, ":010300000001FA\r\n"));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(comm_errors(fd, 1) == 1);
	ASSERT_EQ(comm_errors(fd, 2) == 1);
	close(fd);
	server_stop();
}

TEST(AsciiUdpMalformedDatagramsAreSilentCountedAndNotResynchronized)
{
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 7)))\n");
	fd = client_open();
	ASSERT(fd >= 0);

	/* Valid frame without CR LF, with only CR, and with trailing bytes. */
	ASSERT_EQ(send_raw(fd, ":010300000001FB"));
	ASSERT_EQ(send_raw(fd, ":010300000001FB\r"));
	ASSERT_EQ(send_raw(fd, ":010300000001FB\r\nX"));
	/* Two complete frames in one datagram, and leading garbage before the colon. */
	ASSERT_EQ(send_raw(fd, ":010300000001FB\r\n:010300000001FB\r\n"));
	ASSERT_EQ(send_raw(fd, "x:010300000001FB\r\n"));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(comm_errors(fd, 1) == 5);

	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 7);
	close(fd);
	server_stop();
}

TEST(AsciiUdpUnknownUnitAndBroadcastAreSilentAndUncounted)
{
	u8  write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x2A };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_request(fd, 9, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(send_request(fd, 0, write_pdu, sizeof(write_pdu)));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 0);
	ASSERT_EQ(comm_errors(fd, 1) == 0);
	close(fd);
	server_stop();
}

TEST(AsciiUdpRejectsChangeAsciiDelimiter)
{
	u8  req[] = { FC_DIAGNOSTICS, 0x00, 0x03, 0x0A, 0x00 };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(exchange(fd, 1, req, sizeof(req), resp, &len));
	ASSERT_EQ(len == 2 && resp[0] == (FC_DIAGNOSTICS | 0x80) && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	close(fd);
	server_stop();
}

TEST(AsciiUdpDropsOversizedDatagram)
{
	char big[MODBUS_ASCII_MAX_FRAME + 100];
	u8   resp[MODBUS_MAX_PDU];
	u32  len = 0;
	int  fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 7)))\n");
	fd = client_open();
	ASSERT(fd >= 0);

	/* A valid frame followed by padding that overflows the receive buffer: the
	 * truncated datagram must be dropped, not answered from its prefix. */
	memset(big, '0', sizeof(big));
	memcpy(big, ":010300000001FB\r\n", 17);
	ASSERT_EQ(send(fd, big, sizeof(big), 0) == (ssize_t)sizeof(big));
	ASSERT_EQ(silent(fd));

	ASSERT_EQ(exchange(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 7);
	close(fd);
	server_stop();
}

TEST(AsciiUdpDropFaultStillAppliesWrites)
{
	u8  write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x04, 0xD2 };
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;
	int fd;

	/* Device 1 drops every response; device 2 proves liveness. */
	server_start("", "(device (unit-id 1) (holding 0 (static 0))\n"
			 "  (fault (kind drop) (percent 100)))\n"
			 "(device (unit-id 2) (holding 0 (static 5)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_request(fd, 1, write_pdu, sizeof(write_pdu)));
	ASSERT_EQ(silent(fd));
	ASSERT_EQ(exchange(fd, 2, READ_HOLDING_0, sizeof(READ_HOLDING_0), resp, &len));
	ASSERT_EQ(get_be16(resp + 2) == 5);
	close(fd);
	server_stop();
}

TEST(AsciiUdpChecksumCorruptFaultDamagesLrcOnly)
{
	u8	  raw[MODBUS_ASCII_MAX_FRAME];
	u8	  decoded[MODBUS_MAX_PDU + 2];
	u8	  addr    = 0;
	const u8* pdu     = NULL;
	u32	  pdu_len = 0;
	s32	  got;
	int	  fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 9))\n"
			 "  (fault (kind corrupt) (mode checksum) (percent 100)))\n");
	fd = client_open();
	ASSERT(fd >= 0);
	ASSERT_EQ(send_request(fd, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	got = ts_udp_recv(fd, raw, sizeof(raw), RECV_TIMEOUT_MS);
	ASSERT_EQ(got == 15 && raw[0] == ':' && memcmp(raw + 1, "01030200", 8) == 0);
	ASSERT_EQ(!modbus_ascii_decode(raw, (u32)got, decoded, &addr, &pdu, &pdu_len));
	close(fd);
	server_stop();
}

/* A second server on a port already bound must fail to start (exit 2), not share it. */
TEST(AsciiUdpPortInUseFailsToStart)
{
	const char* bin    = getenv("MOGBUS_BIN");
	ts_server_t second = { .pid = -1, .master_fd = -1, .label = "ascii-udp second",
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
