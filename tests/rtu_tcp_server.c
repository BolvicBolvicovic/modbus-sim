#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "tests/tests_utils/test_support.h"
#include "util/clock.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Integration tests for RTU-over-TCP. Each test starts a real server on a dynamic
 * loopback port and the fixture guarantees cleanup on failure (PRE_TEST/atexit),
 * so a failing ASSERT cannot leave an orphan server or temp config behind.
 * Readiness comes from stderr log polling: no probe client consumes a slot. */

static ts_server_t g_fx = {
	.pid		= -1,
	.master_fd	= -1,
	.probe_family	= AF_INET,
	.flags		= TS_CAPTURE_STDERR | TS_DEATH_SIGNAL | TS_CLEAN_EXIT,
	.label		= "rtu-tcp test",
	.ready_needle	= "listening on"
};

static bool
send_all_fd(int fd, const u8* buf, u32 len)
{
	return ts_send_socket(fd, buf, len);
}

static bool
recv_rtu_resp(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	return ts_rtu_recv(fd, addr, pdu, pdu_len, timeout_ms, true);
}

static bool
wait_for_eof_fd(int fd, u32 timeout_ms)
{
	return ts_wait_eof(fd, timeout_ms);
}

static void
write_cfg_with_bind(const char* bind, const char* server_options, const char* devices)
{
	char config[2048];
	int  n;

	n = snprintf(config, sizeof(config),
		     "(server (mode rtu-tcp) (port %u) (bind \"%s\") %s)\n%s\n",
		     g_fx.probe_port, bind ? bind : "127.0.0.1",
		     server_options ? server_options : "", devices);
	ASSERT(n > 0 && (size_t)n < sizeof(config));
	ASSERT_EQ(ts_write_config(&g_fx, "/tmp/modbus-sim-rtutcp-XXXXXX", config));
}

static void
write_cfg(const char* server_options, const char* devices)
{
	write_cfg_with_bind("127.0.0.1", server_options, devices);
}

static void
server_start_with_bind(const char* bind, int family, const char* server_options,
		       const char* devices)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char*       argv[3];

	g_fx.probe_port   = ts_free_port();
	g_fx.probe_family = family;
	ASSERT(g_fx.probe_port != 0);
	write_cfg_with_bind(bind, server_options, devices);

	argv[0] = (char*)(bin ? bin : "./modbus-sim");
	argv[1] = g_fx.config;
	argv[2] = NULL;
	ASSERT_EQ(ts_server_start(&g_fx, argv));
}

static void
server_start(const char* server_options, const char* devices)
{
	server_start_with_bind("127.0.0.1", AF_INET, server_options, devices);
}

static void
server_start_v6(const char* server_options, const char* devices)
{
	server_start_with_bind("::1", AF_INET6, server_options, devices);
}

static int
connect_port(u32 tries, u32 timeout_ms)
{
	(void)tries;	/* readiness is established by stderr polling at startup */
	return ts_connect_loopback(g_fx.probe_port, AF_INET, timeout_ms);
}

static int
connect_port_v6(u32 tries, u32 timeout_ms)
{
	(void)tries;
	return ts_connect_loopback(g_fx.probe_port, AF_INET6, timeout_ms);
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

TEST(RtuOverTcpServesReadHoldingRegisters)
{
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("", "(device (unit-id 1) (holding 0 (static 4660)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(plen == 4);
	ASSERT_EQ(get_be16(resp + 2) == 4660);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpWriteThenRead)
{
	int fd;
	u8 write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x01, 0x00, 0x2A };
	u8 read_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x01, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("", "(device (unit-id 7)\n"
		     "  (holding 0 (static 0))\n"
		     "  (holding 1 (static 0)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 7, write_pdu, sizeof(write_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 7);
	ASSERT_EQ(resp[0] == FC_WRITE_SINGLE_REGISTER);

	flen = modbus_rtu_encode(frame, 7, read_pdu, sizeof(read_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 42);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpBadCrcGetsNoResponse)
{
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("", "(device (unit-id 1) (holding 0 (static 1)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	frame[flen - 1] ^= 0xFF;
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 250) == false);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpUnknownUnitGetsNoResponse)
{
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("", "(device (unit-id 1) (holding 0 (static 1)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 99, req_pdu, sizeof(req_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 250) == false);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpBroadcastGetsNoResponse)
{
	int fd;
	u8 req_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("(baud 9600)", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 0, req_pdu, sizeof(req_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 250) == false);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpBroadcastExecuteWritesAllDevicesWithoutResponse)
{
	int fd;
	u8 write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x2A };
	u8 read_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("(broadcast-execute on)",
		     "(device (unit-id 1) (holding 0 (static 0)))\n"
		     "(device (unit-id 2) (holding 0 (static 0)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 0, write_pdu, sizeof(write_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 250) == false);

	for (u8 unit = 1; unit <= 2; unit++)
	{
		flen = modbus_rtu_encode(frame, unit, read_pdu, sizeof(read_pdu));
		ASSERT_EQ(send_all_fd(fd, frame, flen));
		ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
		ASSERT_EQ(addr == unit);
		ASSERT_EQ(get_be16(resp + 2) == 42);
	}

	close(fd);
	server_stop();
}

TEST(RtuOverTcpHalfCloseAfterRequestSendsResponse)
{
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("", "(device (unit-id 1) (holding 0 (static 4660)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(shutdown(fd, SHUT_WR) == 0);
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(plen == 4);
	ASSERT_EQ(get_be16(resp + 2) == 4660);
	ASSERT_EQ(wait_for_eof_fd(fd, 2000));

	close(fd);
	server_stop();
}

TEST(RtuOverTcpHalfCloseAfterDelayedRequestSendsResponse)
{
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u64 start;

	server_start("", "(device (unit-id 1)\n"
		     "  (holding 0 (static 4660))\n"
		     "  (fault (kind delay) (delay-ms 200) (percent 100)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	start = (u64)clock_now_ms();
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(shutdown(fd, SHUT_WR) == 0);
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(clock_now_ms() - start >= 180);
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(plen == 4);
	ASSERT_EQ(get_be16(resp + 2) == 4660);
	ASSERT_EQ(wait_for_eof_fd(fd, 2000));

	close(fd);
	server_stop();
}

TEST(RtuOverTcpFragmentedRequest)
{
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("", "(device (unit-id 1) (holding 0 (static 4660)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	for (u32 i = 0; i < flen; i++)
	{
		ASSERT_EQ(send_all_fd(fd, frame + i, 1));
		usleep(2000);
	}
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 4660);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpPipelinedRequests)
{
	int fd;
	u8 req1[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 req2[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x01, 0x00, 0x01 };
	u8 frame1[MODBUS_RTU_MAX_FRAME], frame2[MODBUS_RTU_MAX_FRAME];
	u8 all[MODBUS_RTU_MAX_FRAME * 2], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, len1, len2;

	server_start("", "(device (unit-id 1)\n"
		     "  (holding 0 (static 4660))\n"
		     "  (holding 1 (static 22136)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	len1 = modbus_rtu_encode(frame1, 1, req1, sizeof(req1));
	len2 = modbus_rtu_encode(frame2, 1, req2, sizeof(req2));
	memcpy(all, frame1, len1);
	memcpy(all + len1, frame2, len2);
	ASSERT_EQ(send_all_fd(fd, all, len1 + len2));

	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 4660);
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 22136);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpPipelinedRequestsWithBaudGetResponses)
{
	/* Acceptance: two pipelined requests 3 ms apart over RTU-over-TCP with
	 * (baud 9600) configured are both answered. TCP read() boundaries must not trigger t1.5. */
	int fd;
	u8 req1[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 req2[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x01, 0x00, 0x01 };
	u8 frame1[MODBUS_RTU_MAX_FRAME], frame2[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, len1, len2;

	server_start("(baud 9600)",
		     "(device (unit-id 1)\n"
		     "  (holding 0 (static 4660))\n"
		     "  (holding 1 (static 22136)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	len1 = modbus_rtu_encode(frame1, 1, req1, sizeof(req1));
	len2 = modbus_rtu_encode(frame2, 1, req2, sizeof(req2));
	ASSERT(len1 > 0 && len2 > 0);
	ASSERT_EQ(send_all_fd(fd, frame1, len1));
	usleep(3000);
	ASSERT_EQ(send_all_fd(fd, frame2, len2));

	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 4660);

	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 22136);

	close(fd);
	server_stop();
}

TEST(RtuOverTcpDelayFaultDoesNotBlockOtherClients)
{
	int delayed, fast;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u64 start, fast_ms, delayed_ms;

	server_start("", "(device (unit-id 1) (holding 0 (static 11))\n"
		     "  (fault (kind delay) (delay-ms 500) (percent 100)))\n"
		     "(device (unit-id 2) (holding 0 (static 22)))\n");
	delayed = connect_port(100, 2000);
	fast = connect_port(100, 2000);
	ASSERT(delayed >= 0 && fast >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT_EQ(send_all_fd(delayed, frame, flen));
	flen = modbus_rtu_encode(frame, 2, req_pdu, sizeof(req_pdu));
	ASSERT_EQ(send_all_fd(fast, frame, flen));
	start = (u64)clock_now_ms();

	ASSERT_EQ(recv_rtu_resp(fast, &addr, resp, &plen, 2000));
	fast_ms = clock_now_ms() - start;
	ASSERT_EQ(addr == 2);
	ASSERT_EQ(get_be16(resp + 2) == 22);

	ASSERT_EQ(recv_rtu_resp(delayed, &addr, resp, &plen, 2000));
	delayed_ms = clock_now_ms() - start;
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(get_be16(resp + 2) == 11);
	ASSERT_EQ(fast_ms < 300);
	ASSERT_EQ(delayed_ms >= 400);

	close(delayed);
	close(fast);
	server_stop();
}

TEST(RtuOverTcpDropFaultStillAppliesWrites)
{
	int fd;
	u8 write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x04, 0xD2 }; /* 1234 */
	u8 read_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	bool dropped = false;

	server_start("(seed 1)",
		     "(device (unit-id 1) (holding 0 (static 0))\n"
		     "  (fault (kind drop) (percent 50)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	for (u32 i = 0; i < 40 && !dropped; i++)
	{
		flen = modbus_rtu_encode(frame, 1, write_pdu, sizeof(write_pdu));
		ASSERT_EQ(send_all_fd(fd, frame, flen));
		dropped = !recv_rtu_resp(fd, &addr, resp, &plen, 120);
	}
	ASSERT_EQ(dropped);

	for (u32 i = 0; i < 40; i++)
	{
		flen = modbus_rtu_encode(frame, 1, read_pdu, sizeof(read_pdu));
		ASSERT_EQ(send_all_fd(fd, frame, flen));
		if (recv_rtu_resp(fd, &addr, resp, &plen, 300))
		{
			ASSERT_EQ(get_be16(resp + 2) == 1234);
			close(fd);
			server_stop();
			return;
		}
	}
	ASSERT(false && "did not observe write effect through drop fault");
}

TEST(RtuOverTcpIdleTimeoutClosesConnection)
{
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	int probe;

	server_start("(idle-timeout-ms 100)",
		     "(device (unit-id 1) (holding 0 (static 5)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(wait_for_eof_fd(fd, 2000));

	/* Server should still accept a new connection after idle close. */
	probe = connect_port(100, 2000);
	ASSERT(probe >= 0);
	ASSERT_EQ(send_all_fd(probe, frame, flen));
	ASSERT_EQ(recv_rtu_resp(probe, &addr, resp, &plen, 2000));
	ASSERT_EQ(get_be16(resp + 2) == 5);

	close(probe);
	close(fd);
	server_stop();
}

TEST(RtuOverTcpConnectionLimit)
{
	int clients[32];
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen, opened = 0;
	bool refused = false;

	for (u32 i = 0; i < 32; i++)
		clients[i] = -1;
	server_start("", "(device (unit-id 1) (holding 0 (static 5)))\n");
	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));

	for (u32 i = 0; i < 32; i++)
	{
		clients[i] = connect_port(50, 2000);
		if (clients[i] < 0)
			break;
		opened++;
		usleep(10000);
	}
	ASSERT_EQ(opened == 32);
	ASSERT_EQ(send_all_fd(clients[0], frame, flen));
	ASSERT_EQ(recv_rtu_resp(clients[0], &addr, resp, &plen, 2000));

	/* 33rd connection may be refused, closed, or left unserviced; it must not be
	 * served while 32 connections are already active. */
	{
		int extra = connect_port(1, 200);
		if (extra < 0)
			refused = true;
		else
		{
			refused = !send_all_fd(extra, frame, flen) ||
				  !recv_rtu_resp(extra, &addr, resp, &plen, 300);
			close(extra);
		}
	}
	ASSERT_EQ(refused);

	for (u32 i = 0; i < opened; i++)
		if (clients[i] >= 0)
			close(clients[i]);
	server_stop();
}

TEST(RtuOverTcpLargeFc16Frame)
{
	/* Max-size FC16 request (123 registers) over RTU-over-TCP. */
	int fd;
	u8 req_pdu[1 + 2 + 2 + 1 + 123 * 2];
	u8 read_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x7A, 0x00, 0x01 }; /* reg 122 */
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;

	server_start("", "(device (unit-id 1) (holding 0 (count 125) (static 0)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);

	req_pdu[0] = FC_WRITE_MULTIPLE_REGISTERS;
	put_be16(req_pdu + 1, 0);
	put_be16(req_pdu + 3, 123);
	req_pdu[5] = 246;
	for (u32 i = 0; i < 123; i++)
		put_be16(req_pdu + 6 + i * 2, (u16)(0x2000 + i));
	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(resp[0] == FC_WRITE_MULTIPLE_REGISTERS);
	ASSERT_EQ(get_be16(resp + 3) == 123);

	flen = modbus_rtu_encode(frame, 1, read_pdu, sizeof(read_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(get_be16(resp + 2) == 0x2000 + 122);

	close(fd);
	server_stop();
}

static void
assert_rtu_tcp_fc16_split_answered(u32 split_after)
{
	u8 resp[MODBUS_MAX_PDU];
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 pdu[6 + 8 * 2];
	u32 plen = 0, flen;
	u8 addr = 0;
	int fd;

	pdu[0] = FC_WRITE_MULTIPLE_REGISTERS;
	put_be16(pdu + 1, 0);
	put_be16(pdu + 3, 8);
	pdu[5] = 16;
	for (u32 i = 0; i < 8; i++)
		put_be16(pdu + 6 + i * 2, (u16)(0x6000 + i));

	server_start("(baud 9600)",
		     "(device (unit-id 1) (holding 0 (count 16) (static 0)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);
	flen = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT(flen > split_after);
	ASSERT_EQ(send_all_fd(fd, frame, split_after));
	usleep(6000);
	ASSERT_EQ(send_all_fd(fd, frame + split_after, flen - split_after));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	close(fd);
	server_stop();
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_WRITE_MULTIPLE_REGISTERS);
	ASSERT_EQ(get_be16(resp + 1) == 0);
	ASSERT_EQ(get_be16(resp + 3) == 8);
}

TEST(RtuOverTcpIncompleteFc16SplitAfter5Answered)
{
	assert_rtu_tcp_fc16_split_answered(5);
}

TEST(RtuOverTcpIncompleteFc16SplitAfter7Answered)
{
	assert_rtu_tcp_fc16_split_answered(7);
}

TEST(RtuOverTcpIncompleteFc16SplitAfter20Answered)
{
	assert_rtu_tcp_fc16_split_answered(20);
}

TEST(RtuOverTcpIncompleteFc08SplitAnswered)
{
	u8 resp[MODBUS_MAX_PDU];
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 pdu[] = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34 };
	u32 plen = 0, flen;
	u8 addr = 0;
	int fd;

	server_start("", "(device (unit-id 1) (holding 0 (static 0)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);
	flen = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT(flen > 4);
	ASSERT_EQ(send_all_fd(fd, frame, 4));
	usleep(6000);
	ASSERT_EQ(send_all_fd(fd, frame + 4, flen - 4));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	close(fd);
	server_stop();
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(plen == sizeof(pdu));
	ASSERT_EQ(memcmp(resp, pdu, sizeof(pdu)) == 0);
}

TEST(RtuOverTcpTruncatedFrameDoesNotLoseValidRequest)
{
	u8 resp[MODBUS_MAX_PDU];
	u8 good[MODBUS_RTU_MAX_FRAME];
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 trunc[] = { 0x01, FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00 };
	u32 plen = 0, good_len;
	u8 addr = 0;
	int fd;

	server_start("(baud 9600)", "(device (unit-id 1) (holding 0 (static 4386)))\n");
	fd = connect_port(100, 2000);
	ASSERT(fd >= 0);
	good_len = modbus_rtu_encode(good, 1, req, sizeof(req));
	ASSERT_EQ(send_all_fd(fd, trunc, sizeof(trunc)));
	usleep(10000);
	ASSERT_EQ(send_all_fd(fd, good, good_len));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	close(fd);
	server_stop();
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 4386);
}

TEST(RtuOverTcpIpv6LoopbackServesRequest)
{
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	int fd;

	if (!ts_ipv6_loopback_available())
	{
		fprintf(stderr, "SKIP: IPv6 loopback unavailable\n");
		return;
	}

	server_start_v6("", "(device (unit-id 1) (holding 0 (static 4660)))\n");
	fd = connect_port_v6(100, 2000);
	ASSERT(fd >= 0);
	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(plen == 4);
	ASSERT_EQ(get_be16(resp + 2) == 4660);

	close(fd);
	server_stop();
}
