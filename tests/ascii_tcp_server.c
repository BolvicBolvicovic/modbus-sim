#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "tests/tests_utils/test_support.h"
#include "util/clock.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Integration tests for raw Modbus ASCII over TCP. Each test starts the real
 * server binary on a dynamic loopback port; PRE_TEST guarantees cleanup even
 * when an assertion fails. Readiness is detected on stderr, without a probe
 * client that would occupy a connection slot. */

static ts_server_t g_fx = {
	.pid		= -1,
	.master_fd	= -1,
	.probe_family	= AF_INET,
	.flags		= TS_CAPTURE_STDERR | TS_DEATH_SIGNAL | TS_CLEAN_EXIT,
	.label		= "ascii-tcp test",
	.ready_needle	= "listening on"
};

static bool
send_all_fd(int fd, const u8* buf, u32 len)
{
	return ts_send_socket(fd, buf, len);
}

static bool
recv_ascii_frame(int fd, u8* frame, u32* len, u32 timeout_ms)
{
	return ts_ascii_recv_frame(fd, frame, len, timeout_ms, true);
}

static bool
recv_ascii_response(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	return ts_ascii_recv_response(fd, addr, pdu, pdu_len, timeout_ms, true);
}

static bool
recv_nothing(int fd, u32 timeout_ms)
{
	return ts_no_data(fd, timeout_ms);
}

static bool
send_ascii_request(int fd, u8 unit, const u8* pdu, u32 pdu_len)
{
	return ts_ascii_send(fd, unit, pdu, pdu_len, true);
}

static void
write_cfg_with_bind(const char* bind, const char* options, const char* devices)
{
	char config[2048];
	int  n;

	n = snprintf(config, sizeof(config),
		     "(server (mode ascii-tcp) (port %u) (bind \"%s\") %s)\n%s\n",
		     g_fx.probe_port, bind, options ? options : "", devices);
	ASSERT(n > 0 && (size_t)n < sizeof(config));
	ASSERT_EQ(ts_write_config(&g_fx, "/tmp/mogbus-asciitcp-XXXXXX", config));
}

static void
server_start_with_bind(const char* bind, int family, const char* options, const char* devices)
{
	const char* bin = getenv("MOGBUS_BIN");
	char*       argv[3];

	g_fx.probe_port   = ts_free_port();
	g_fx.probe_family = family;
	ASSERT(g_fx.probe_port != 0);
	write_cfg_with_bind(bind, options, devices);

	argv[0] = (char*)(bin ? bin : "./mogbus");
	argv[1] = g_fx.config;
	argv[2] = NULL;
	ASSERT_EQ(ts_server_start(&g_fx, argv));
}

static void
server_start(const char* options, const char* devices)
{
	server_start_with_bind("127.0.0.1", AF_INET, options, devices);
}

static void
server_start_v6(void)
{
	server_start_with_bind("::1", AF_INET6, "",
			       "(device (unit-id 1) (holding 0 (static 4660)))");
}

static int
connect_port(int family, u32 tries, u32 timeout_ms)
{
	(void)tries;	/* readiness is established by stderr polling at startup */
	return ts_connect_loopback(g_fx.probe_port, family, timeout_ms);
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

SETUP_SUITE()
{
	(void)_ss_arena;
	atexit(server_cleanup);
}

PRE_TEST()
{
	server_stop();
}

TEST(AsciiTcpServesReadHoldingRegisters)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 pdu[MODBUS_MAX_PDU], addr = 0;
	u32 pdu_len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 4660)))");
	fd = connect_port(AF_INET, 100, 2000);
	ASSERT(fd >= 0);
	ASSERT_EQ(send_ascii_request(fd, 1, req, sizeof(req)));
	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 2000));
	ASSERT_EQ(addr == 1 && pdu[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(pdu_len == 4 && get_be16(pdu + 2) == 4660);
	close(fd);
	server_stop();
}

TEST(AsciiTcpAnswersTwoFramesInOneWrite)
{
	u8 req1[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 req2[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x01, 0x00, 0x01 };
	u8 frames[MODBUS_ASCII_MAX_FRAME * 2], pdu[MODBUS_MAX_PDU], addr = 0;
	u32 len1, len2, pdu_len = 0;
	int fd;

	server_start("", "(device (unit-id 1)\n"
		     "  (holding 0 (static 4660))\n"
		     "  (holding 1 (static 22136)))");
	fd = connect_port(AF_INET, 100, 2000);
	ASSERT(fd >= 0);
	len1 = modbus_ascii_encode(frames, 1, req1, sizeof(req1));
	len2 = modbus_ascii_encode(frames + len1, 1, req2, sizeof(req2));
	ASSERT(len1 > 0 && len2 > 0);
	ASSERT_EQ(send_all_fd(fd, frames, len1 + len2));

	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 2000));
	ASSERT_EQ(addr == 1 && pdu_len == 4 && get_be16(pdu + 2) == 4660);
	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 2000));
	ASSERT_EQ(addr == 1 && pdu_len == 4 && get_be16(pdu + 2) == 22136);
	close(fd);
	server_stop();
}

TEST(AsciiTcpWaitsForLfAfterTrailingCr)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_ASCII_MAX_FRAME], pdu[MODBUS_MAX_PDU], addr = 0;
	u32 len, pdu_len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 4660)))");
	fd = connect_port(AF_INET, 100, 2000);
	ASSERT(fd >= 0);
	len = modbus_ascii_encode(frame, 1, req, sizeof(req));
	ASSERT(len > 1);
	ASSERT_EQ(send_all_fd(fd, frame, len - 1)); /* CR, but no LF yet */
	usleep(100000);
	ASSERT_EQ(recv_nothing(fd, 100));
	ASSERT_EQ(send_all_fd(fd, frame + len - 1, 1));
	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 2000));
	ASSERT_EQ(addr == 1 && get_be16(pdu + 2) == 4660);
	close(fd);
	server_stop();
}

TEST(AsciiTcpBadLrcIsSilentAndCountsForEveryDevice)
{
	const char bad_lrc[] = ":010300000001FA\r\n";
	u8 diag[] = { FC_DIAGNOSTICS, 0x00, DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT, 0x00, 0x00 };
	u8 pdu[MODBUS_MAX_PDU], addr = 0;
	u32 pdu_len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 1)))\n"
		     "(device (unit-id 2) (holding 0 (static 2)))");
	fd = connect_port(AF_INET, 100, 2000);
	ASSERT(fd >= 0);
	ASSERT_EQ(send_all_fd(fd, (const u8*)bad_lrc, sizeof(bad_lrc) - 1));
	ASSERT_EQ(recv_nothing(fd, 250));

	for (u8 unit = 1; unit <= 2; unit++)
	{
		ASSERT_EQ(send_ascii_request(fd, unit, diag, sizeof(diag)));
		ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 2000));
		ASSERT_EQ(addr == unit && pdu[0] == FC_DIAGNOSTICS);
		ASSERT_EQ(pdu_len == 5 && get_be16(pdu + 1) == DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT);
		ASSERT_EQ(get_be16(pdu + 3) == 1);
	}
	close(fd);
	server_stop();
}

TEST(AsciiTcpUnitZeroBroadcastIsNotProcessed)
{
	u8 write_req[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x2A };
	u8 read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 pdu[MODBUS_MAX_PDU], addr = 0;
	u32 pdu_len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))");
	fd = connect_port(AF_INET, 100, 2000);
	ASSERT(fd >= 0);
	ASSERT_EQ(send_ascii_request(fd, 0, write_req, sizeof(write_req)));
	ASSERT_EQ(recv_nothing(fd, 250));
	ASSERT_EQ(send_ascii_request(fd, 1, read_req, sizeof(read_req)));
	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 2000));
	ASSERT_EQ(addr == 1 && get_be16(pdu + 2) == 0);
	close(fd);
	server_stop();
}

TEST(AsciiTcpDelayedFaultDoesNotExpireQueuedFrame)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frames[MODBUS_ASCII_MAX_FRAME * 2], pdu[MODBUS_MAX_PDU], addr = 0;
	u32 len1, len2, pdu_len = 0;
	u64 start_ms, first_resp_ms, second_resp_ms;
	int fd;

	/* The second frame waits behind a >1 s delayed response. Its byte idle
	 * time exceeds the partial-frame timeout, but it is complete and intact. */
	server_start("", "(device (unit-id 1) (holding 0 (static 42))\n"
		     "  (fault (kind delay) (delay-ms 1200) (percent 100)))");
	fd = connect_port(AF_INET, 100, 2000);
	ASSERT(fd >= 0);
	len1 = modbus_ascii_encode(frames, 1, req, sizeof(req));
	len2 = modbus_ascii_encode(frames + len1, 1, req, sizeof(req));
	ASSERT(len1 > 0 && len2 > 0);
	start_ms = (u64)clock_now_ms();
	ASSERT_EQ(send_all_fd(fd, frames, len1 + len2));

	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 3000));
	first_resp_ms = (u64)clock_now_ms();
	ASSERT_EQ(first_resp_ms - start_ms >= 1100 && get_be16(pdu + 2) == 42);
	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 3000));
	second_resp_ms = (u64)clock_now_ms();
	ASSERT_EQ(addr == 1 && get_be16(pdu + 2) == 42);
	ASSERT_EQ(second_resp_ms - first_resp_ms >= 1000 && second_resp_ms - first_resp_ms < 1600);
	close(fd);
	server_stop();
}

TEST(AsciiTcpDisconnectFaultClosesClient)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 pdu[MODBUS_MAX_PDU], addr = 0;
	u32 pdu_len = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 42))\n"
		     "  (fault (kind disconnect) (percent 100)))");
	fd = connect_port(AF_INET, 100, 2000);
	ASSERT(fd >= 0);
	ASSERT_EQ(send_ascii_request(fd, 1, req, sizeof(req)));
	ASSERT_EQ(ts_wait_eof(fd, 2000));
	ASSERT_EQ(!recv_ascii_response(fd, &addr, pdu, &pdu_len, 200));
	close(fd);
	server_stop();
}

TEST(AsciiTcpServesIpv6Loopback)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 pdu[MODBUS_MAX_PDU], addr = 0;
	u32 pdu_len = 0;
	int fd;

	if (!ts_ipv6_loopback_available())
		return; /* IPv6 loopback is unavailable in this environment. */
	server_start_v6();
	fd = connect_port(AF_INET6, 100, 2000);
	ASSERT(fd >= 0);
	ASSERT_EQ(send_ascii_request(fd, 1, req, sizeof(req)));
	ASSERT_EQ(recv_ascii_response(fd, &addr, pdu, &pdu_len, 2000));
	ASSERT_EQ(addr == 1 && get_be16(pdu + 2) == 4660);
	close(fd);
	server_stop();
}
