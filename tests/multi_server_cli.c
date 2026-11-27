/* A config with several servers: the real binary starts them all, runs them together and answers
 * on each. Every test picks its own free ports, so tests may run in parallel. */
#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "tests/tests_utils/test_support.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static ts_server_t g_multi = {
	.pid		= -1,
	.master_fd	= -1,
	.flags		= TS_CAPTURE_STDERR | TS_NULL_STDIN | TS_NULL_STDOUT | TS_DEATH_SIGNAL | TS_CLEAN_EXIT,
	.label		= "multi server",
	.ready_needle	= "listening on UDP"	/* the last server of the config */
};

static const u8 READ_HOLDING_0[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };

SETUP_SUITE()
{
	(void)_ss_arena;
}

PRE_TEST()
{
	ts_server_cleanup(&g_multi);
}

static const char*
mogbus_bin(void)
{
	const char* bin = getenv("MOGBUS_BIN");

	return bin ? bin : "./mogbus";
}

/* Writes the three-server config (tcp, rtu-tcp, udp) and returns the ports through the arguments. */
static bool
write_config(u16* tcp_port, u16* rtu_port, u16* udp_port)
{
	char config[1024];

	*tcp_port = ts_free_port();
	*rtu_port = ts_free_port();
	*udp_port = ts_free_port();
	if (!*tcp_port || !*rtu_port || !*udp_port)
		return false;
	g_multi.probe_port = *tcp_port;
	snprintf(config, sizeof(config),
		 "(server (id \"plant\") (mode tcp) (port %u))\n"
		 "(server (id \"gateway\") (mode rtu-tcp) (port %u))\n"
		 "(server (id \"legacy\") (mode udp) (port %u))\n"
		 "(device (name \"meter\") (unit-id 1) (servers \"plant\" \"gateway\") (holding 0 (static 11)))\n"
		 "(device (name \"sensor\") (unit-id 2) (servers \"legacy\") (holding 0 (static 22)))\n",
		 *tcp_port, *rtu_port, *udp_port);
	return ts_write_config(&g_multi, "/tmp/mogbus-multi-XXXXXX", config);
}

static bool
start(char* const extra[])
{
	char*	argv[6];
	int	argc = 0;

	argv[argc++] = (char*)mogbus_bin();
	for (int i = 0; extra && extra[i]; i++)
		argv[argc++] = extra[i];
	argv[argc++] = g_multi.config;
	argv[argc]   = NULL;
	return ts_server_start(&g_multi, argv);
}

TEST(MultiServerAnswersOnEveryTransport)
{
	u16	tcp_port, rtu_port, udp_port;
	u8	resp[MODBUS_MAX_PDU];
	u32	len = 0;
	int	tcp, rtu, udp;
	u8	addr = 0;
	u16	tid;

	ASSERT_EQ(write_config(&tcp_port, &rtu_port, &udp_port));
	ASSERT_EQ(start(NULL));

	tcp = ts_connect_loopback(tcp_port, AF_INET, 1000);
	rtu = ts_connect_loopback(rtu_port, AF_INET, 1000);
	udp = ts_udp_loopback(udp_port, AF_INET, 1000);
	ASSERT(tcp >= 0 && rtu >= 0 && udp >= 0);

	tid = ts_next_tid();
	ASSERT_EQ(ts_mbap_send(tcp, 1, tid, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(ts_mbap_recv(tcp, 1, tid, resp, &len, 1000));
	ASSERT_EQ(len == 4 && get_be16(resp + 2) == 11);

	ASSERT_EQ(ts_rtu_send(rtu, 1, READ_HOLDING_0, sizeof(READ_HOLDING_0), true));
	ASSERT_EQ(ts_rtu_recv(rtu, &addr, resp, &len, 1000, true));
	ASSERT_EQ(addr == 1 && len == 4 && get_be16(resp + 2) == 11);

	tid = ts_next_tid();
	ASSERT_EQ(ts_mbap_send(udp, 2, tid, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(ts_udp_mbap_recv(udp, 2, tid, resp, &len, 1000));
	ASSERT_EQ(len == 4 && get_be16(resp + 2) == 22);

	/* Unit 2 is only on the UDP server: the TCP server answers the gateway exception. */
	tid = ts_next_tid();
	ASSERT_EQ(ts_mbap_send(tcp, 2, tid, READ_HOLDING_0, sizeof(READ_HOLDING_0)));
	ASSERT_EQ(ts_mbap_recv(tcp, 2, tid, resp, &len, 1000));
	ASSERT_EQ(len == 2 && resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80) &&
		  resp[1] == EXC_GATEWAY_TARGET_FAILED);

	/* Every server logs with its id as a prefix. */
	ASSERT_EQ(ts_file_contains(g_multi.stderr_path, "[plant] listening on") &&
		  ts_file_contains(g_multi.stderr_path, "[gateway] RTU-over-TCP listening on") &&
		  ts_file_contains(g_multi.stderr_path, "[legacy] listening on UDP"));

	close(tcp);
	close(rtu);
	close(udp);
	ASSERT_EQ(ts_server_stop(&g_multi));
}

/* Runs the binary with the arguments and the config; returns its exit status and output. */
static int
run_mogbus(const char* flags, char* out, size_t cap)
{
	char	cmd[512];
	FILE*	pipe;
	size_t	n;
	int	status;

	snprintf(cmd, sizeof(cmd), "%s %s %s 2>&1", mogbus_bin(), flags, g_multi.config);
	pipe = popen(cmd, "r");
	if (!pipe)
		return -1;
	n = fread(out, 1, cap - 1, pipe);
	out[n] = '\0';
	status = pclose(pipe);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

TEST(MultiServerCheckShowsEachServerAndItsDevices)
{
	u16	tcp_port, rtu_port, udp_port;
	char	out[4096];

	ASSERT_EQ(write_config(&tcp_port, &rtu_port, &udp_port));
	ASSERT_EQ(run_mogbus("--check=verbose", out, sizeof(out)) == 0);
	ASSERT_EQ(strstr(out, "[plant] 1 device on 127.0.0.1:") != NULL);
	ASSERT_EQ(strstr(out, "[gateway] 1 device, RTU-over-TCP") != NULL);
	ASSERT_EQ(strstr(out, "[legacy] 1 device, UDP") != NULL);
	ASSERT_EQ(strstr(out, "unit 1   meter") != NULL && strstr(out, "unit 2   sensor") != NULL);
	ASSERT_EQ(strstr(out, "config OK") != NULL);
	ts_server_cleanup(&g_multi);
}

TEST(MultiServerHealthProbesTheFirstOrTheSelectedServer)
{
	u16	tcp_port, rtu_port, udp_port;
	char	out[4096];

	ASSERT_EQ(write_config(&tcp_port, &rtu_port, &udp_port));
	ASSERT_EQ(start(NULL));
	ASSERT_EQ(run_mogbus("--health", out, sizeof(out)) == 0);
	ASSERT_EQ(run_mogbus("--health --server legacy", out, sizeof(out)) == 0);
	ASSERT_EQ(run_mogbus("--health --server gateway", out, sizeof(out)) == 0);
	ASSERT_EQ(run_mogbus("--health --server nope", out, sizeof(out)) == 2);
	ASSERT_EQ(strstr(out, "no server with id 'nope'") != NULL);
	ASSERT_EQ(ts_server_stop(&g_multi));

	/* The stop removed the config; write it again on fresh ports where nothing listens: unhealthy. */
	ASSERT_EQ(write_config(&tcp_port, &rtu_port, &udp_port));
	ASSERT_EQ(run_mogbus("--health --timeout-ms 200 --server legacy", out, sizeof(out)) == 1);
	ts_server_cleanup(&g_multi);
}

TEST(MultiServerRefusesClashingPorts)
{
	char	config[512];
	char	out[4096];
	u16	port = ts_free_port();

	g_multi.probe_port = port;
	snprintf(config, sizeof(config),
		 "(server (id \"a\") (mode tcp) (port %u))\n"
		 "(server (id \"b\") (mode rtu-tcp) (port %u))\n"
		 "(device (unit-id 1) (servers \"a\" \"b\") (holding 0))\n", port, port);
	ASSERT_EQ(ts_write_config(&g_multi, "/tmp/mogbus-multi-XXXXXX", config));
	ASSERT_EQ(run_mogbus("--check", out, sizeof(out)) == 1);
	ASSERT_EQ(strstr(out, "both listen on TCP port") != NULL);
	ts_server_cleanup(&g_multi);
}
