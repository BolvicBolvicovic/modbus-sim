/* Network integration tests: run the real server binary and talk to it over loopback.
 * Each test starts a server on a private port and stops it before asserting, so a failed
 * assertion cannot leave a child behind. PRE_TEST/atexit clean up leftovers from failures.
 * Readiness comes from stderr log polling, so capacity tests never need a probe client. */
#include "vtest.h"
#include "build_config.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "tests/tests_utils/test_support.h"
#if WITH_TLS
#include "tests/tests_utils/tls_client.h"
#endif
#include "util/clock.h"
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#define NET_RECV_TIMEOUT_MS	1000
#define NET_SHORT_TIMEOUT_MS	100
#define NET_MAX_ATTEMPTS	24

typedef struct client_t client_t;
struct client_t
{
	int	fd;
};

static ts_server_t g_server = {
	.pid		= -1,
	.master_fd	= -1,
	.probe_family	= AF_INET,
	.flags		= TS_CAPTURE_STDERR | TS_NULL_STDOUT | TS_CLEAN_EXIT,
	.label		= "net test",
	.ready_needle	= "listening on"
};
static u32 g_server_nofile = 0;

/* Separate fixture for --check runs, which exit instead of listening. */
#if WITH_TLS
static ts_server_t g_tls_check = {
	.pid		= -1,
	.master_fd	= -1,
	.flags		= TS_CAPTURE_STDERR | TS_NULL_STDIN | TS_NULL_STDOUT | TS_EXIT_IS_READY,
	.label		= "tls check",
	.ready_needle	= "config OK"
};
#endif

/* --- socket helpers --- */

static bool
client_open(u16 port, int timeout_ms, client_t* out)
{
	int fd = ts_connect_loopback(port, AF_INET, (u32)timeout_ms);

	if (fd < 0)
		return false;
	out->fd = fd;
	return true;
}

static bool
client_open_v6(u16 port, int timeout_ms, client_t* out)
{
	int fd = ts_connect_loopback(port, AF_INET6, (u32)timeout_ms);

	if (fd < 0)
		return false;
	out->fd = fd;
	return true;
}

static void
client_close(client_t* client)
{
	if (client->fd >= 0)
	{
		close(client->fd);
		client->fd = -1;
	}
}

static bool
send_all(int fd, const u8* buf, u32 len)
{
	return ts_send_socket(fd, buf, len);
}

static bool
send_request(client_t* client, u8 unit, u16 tid, const u8* pdu, u32 pdu_len)
{
	return ts_mbap_send(client->fd, unit, tid, pdu, pdu_len);
}

static bool
recv_reply(client_t* client, u8 unit, u16 tid, u8* pdu, u32* pdu_len)
{
	return ts_mbap_recv(client->fd, unit, tid, pdu, pdu_len, NET_RECV_TIMEOUT_MS);
}

static bool
exchange(client_t* client, u8 unit, const u8* req, u32 req_len, u8* resp, u32* resp_len)
{
	u16 tid = ts_next_tid();

	return send_request(client, unit, tid, req, req_len) &&
	       recv_reply(client, unit, tid, resp, resp_len);
}

#if WITH_TLS
#define TLS_CA_PATH		"tests/tls/ca.pem"
#define TLS_SERVER_CERT_PATH	"tests/tls/server.pem"
#define TLS_SERVER_KEY_PATH	"tests/tls/server.key"
#define TLS_CLIENT_CERT_PATH	"tests/tls/client.pem"
#define TLS_CLIENT_KEY_PATH	"tests/tls/client.key"
#define TLS_PROBE_TIMEOUT_MS	120

static const char	TLS_SERVER_OPTIONS[] =
	"(tls (certificate \"" TLS_SERVER_CERT_PATH "\") "
	"(private-key \"" TLS_SERVER_KEY_PATH "\") "
	"(ca-certificate \"" TLS_CA_PATH "\"))";

static const char	TLS_SHORT_HANDSHAKE_OPTIONS[] =
	"(tls (certificate \"" TLS_SERVER_CERT_PATH "\") "
	"(private-key \"" TLS_SERVER_KEY_PATH "\") "
	"(ca-certificate \"" TLS_CA_PATH "\") "
	"(handshake-timeout-ms 250))";

static const char	TLS_IDLE_SERVER_OPTIONS[] =
	"(tls (certificate \"" TLS_SERVER_CERT_PATH "\") "
	"(private-key \"" TLS_SERVER_KEY_PATH "\") "
	"(ca-certificate \"" TLS_CA_PATH "\")) "
	"(idle-timeout-ms 200)";

/* Runs the real binary with --check and verifies its status and captured output. */
static bool
tls_check_run(const char* tls_options, int expected_status, const char* stderr_needle)
{
	const char* bin   = getenv("MOGBUS_BIN");
	char	    config[512];
	char*	    argv[] = { (char*)(bin ? bin : "./mogbus"), "--check",
			       g_tls_check.config, NULL };
	int	    status = 0;
	bool	    ok;

	if (snprintf(config, sizeof(config),
		     "(server (mode tcp) (bind \"127.0.0.1\") %s)\n"
		     "(device (unit-id 1) (holding 0 (static 1)))\n",
		     tls_options) >= (int)sizeof(config))
		return false;
	if (!ts_write_config(&g_tls_check, "/tmp/mogbus-tlscheck-XXXXXX", config) ||
	    !ts_temp_file(g_tls_check.stderr_path, sizeof(g_tls_check.stderr_path),
			  "/tmp/mogbus-tlscheckerr-XXXXXX") ||
	    !ts_temp_file(g_tls_check.stdout_path, sizeof(g_tls_check.stdout_path),
			  "/tmp/mogbus-tlscheckout-XXXXXX"))
		goto fail;

	g_tls_check.flags = TS_CAPTURE_STDERR | TS_CAPTURE_STDOUT | TS_NULL_STDIN |
			    TS_EXIT_IS_READY;
	if (!ts_server_start(&g_tls_check, argv))
		goto fail;

	ok = ts_wait_pid(g_tls_check.pid, &status, 5000);
	g_tls_check.pid = -1;
	ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == expected_status;
	if (stderr_needle)
		ok = ok && ts_file_contains(g_tls_check.stderr_path, stderr_needle);
	if (expected_status == 0)
		ok = ok && ts_file_contains(g_tls_check.stdout_path, "config OK");
	else
		ok = ok && !ts_file_contains(g_tls_check.stdout_path, "config OK");
	ok = ts_server_stop(&g_tls_check) && ok;
	return ok;

fail:
	ts_server_cleanup(&g_tls_check);
	return false;
}

enum
{
	TLS_PIPELINE_FRAME_LEN = MBAP_HEADER_SIZE + 5,
	TLS_PIPELINE_COUNT     = MBAP_MAX_FRAME / TLS_PIPELINE_FRAME_LEN + 1,
	TLS_PIPELINE_BYTES     = TLS_PIPELINE_COUNT * TLS_PIPELINE_FRAME_LEN,
	TLS_DISCONNECT_FILLERS = 31,	/* server cap is 32; leave one slot for the test client */
	TLS_HANDSHAKE_FILLERS  = 32	/* fill every slot with stalled handshakes */
};
_Static_assert(TLS_PIPELINE_BYTES > MBAP_MAX_FRAME,
	       "pipelined TLS test must exceed the server receive buffer");

static bool
tls_send_request(tls_client_t* client, u8 unit, u16 tid, const u8* pdu, u32 pdu_len)
{
	u8 frame[MBAP_MAX_FRAME];

	return tls_client_write_all(client, frame,
				    ts_mbap_encode(frame, unit, tid, pdu, pdu_len));
}

static bool
tls_recv_reply(tls_client_t* client, u8 unit, u16 tid, u8* pdu, u32* pdu_len)
{
	u8 raw[MBAP_HEADER_SIZE];

	if (!tls_client_read_exact(client, raw, sizeof(raw)))
		return false;
	if (!ts_mbap_reply_header(raw, unit, tid, pdu_len))
		return false;
	return tls_client_read_exact(client, pdu, *pdu_len);
}
#endif

static bool
recv_none(client_t* client, int timeout_ms)
{
	return ts_no_data(client->fd, (u32)timeout_ms);
}

static u32
read_pdu(u8* pdu, u8 function, u16 address, u16 quantity)
{
	pdu[0] = function;
	put_be16(pdu + 1, address);
	put_be16(pdu + 3, quantity);
	return 5;
}

#if WITH_TLS
static void
tls_build_pipeline(u8 (*frames)[TLS_PIPELINE_FRAME_LEN], u16* tids)
{
	for (u32 i = 0; i < TLS_PIPELINE_COUNT; i++)
		ts_mbap_encode(frames[i], 1, tids[i],
			       (const u8[]){ FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 }, 5);
}
#endif

static u32
write_pdu(u8* pdu, u16 address, u16 value)
{
	pdu[0] = FC_WRITE_SINGLE_REGISTER;
	put_be16(pdu + 1, address);
	put_be16(pdu + 3, value);
	return 5;
}

static bool
exception_is(const u8* pdu, u32 len, u8 function, u8 code)
{
	return len == 2 && pdu[0] == (u8)(function | 0x80) && pdu[1] == code;
}

static u16
register_value(const u8* pdu, u32 len, u32 index)
{
	return len >= 2 + (index + 1) * 2 ? get_be16(pdu + 2 + index * 2) : 0;
}

static bool
set_small_send_buffer(int fd, int size)
{
	return setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0;
}

static bool
wait_for_server_close(client_t* client, u32 timeout_ms)
{
	return ts_wait_eof(client->fd, timeout_ms);
}

/* --- server fixture --- */

/* `server_options` is appended inside (server ...); pass "" for defaults. */
static bool
server_start_with_bind(const char* bind, const char* server_options, const char* devices)
{
	char config[2048];
	int  n;

	if (!bind)
		return false;
	g_server.probe_port   = ts_free_port();
	g_server.probe_family = strcmp(bind, "::1") == 0 ? AF_INET6 : AF_INET;
	g_server.nofile       = g_server_nofile;
	if (!g_server.probe_port)
	{
		fprintf(stderr, "net test: no free port\n");
		return false;
	}

	n = snprintf(config, sizeof(config), "(server (mode tcp) (port %u) (bind \"%s\") %s)\n%s\n",
		     g_server.probe_port, bind,
		     server_options ? server_options : "", devices);
	if (n <= 0 || (size_t)n >= sizeof(config) ||
	    !ts_write_config(&g_server, "/tmp/mogbus-net-XXXXXX", config))
		return false;

	{
		const char* bin = getenv("MOGBUS_BIN");
		char*       argv[] = { (char*)(bin ? bin : "./mogbus"), g_server.config, NULL };

		return ts_server_start(&g_server, argv);
	}
}

static bool
server_start(const char* server_options, const char* devices)
{
	return server_start_with_bind("127.0.0.1", server_options, devices);
}

static void
server_stop(void)
{
	ASSERT_EQ(ts_server_stop(&g_server));
	g_server.probe_port = 0;
	g_server_nofile = 0;
}

static void
server_cleanup(void)
{
	ts_server_cleanup(&g_server);
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

/* --- tests --- */

/* Runs `mogbus --health CONFIG` and returns its exit status, or -1 if it did not run. */
static int
health_status(const char* config)
{
	const char* bin    = getenv("MOGBUS_BIN");
	char*	    argv[] = { (char*)(bin ? bin : "./mogbus"), "--health", (char*)config,
			       "--timeout-ms", "1000", NULL };
	int	    status = 0;
	pid_t	    pid    = fork();

	if (pid == 0)
	{
		int null_fd = open("/dev/null", O_WRONLY);

		if (null_fd >= 0)
			dup2(null_fd, STDERR_FILENO);
		execv(argv[0], argv);
		_exit(127);
	}
	if (pid < 0 || !ts_wait_pid(pid, &status, 5000) || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

TEST(NetHealthFlagProbesTheRunningServer)
{
	char	missing[80] = "/tmp/mogbus-health-XXXXXX";
	char	text[160];
	int	fd;
	int	up   = -2;
	int	down = -2;
	bool	ok   = server_start("", "(device (unit-id 7) (holding 0 (static 1)))");

	if (ok)
		up = health_status(g_server.config);
	server_stop();

	fd = mkstemp(missing);
	ok = ok && fd >= 0;
	if (fd >= 0)
	{
		snprintf(text, sizeof(text),
			 "(server (mode tcp) (port %u))\n(device (unit-id 7) (holding 0 (static 1)))\n",
			 ts_free_port());
		ok = ok && write(fd, text, strlen(text)) == (ssize_t)strlen(text);
		close(fd);
		down = health_status(missing);
		unlink(missing);
	}
	ASSERT_EQ(ok && up == 0 && down == 1);
}

TEST(NetPersistentConnectionServesRequestsInOrder)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len  = 0;
	bool	 ok   = server_start("",
				     "(device (name \"basic\") (unit-id 1)\n"
				     "  (holding 0 (static 4660))\n"
				     "  (input 0 (static 7))\n"
				     "  (coil 0 (static on)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     resp[0] == FC_READ_HOLDING_REGISTERS && register_value(resp, len, 0) == 0x1234;
	}
	if (ok)
	{
		read_pdu(req, FC_READ_INPUT_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     resp[0] == FC_READ_INPUT_REGISTERS && register_value(resp, len, 0) == 7;
	}
	if (ok)
	{
		read_pdu(req, FC_READ_COILS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 3 &&
		     resp[0] == FC_READ_COILS && resp[2] == 1;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetPipelinedRequestsGetAnswersInOrder)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len    = 0;
	u16	 first  = ts_next_tid();
	u16	 second = ts_next_tid();
	bool	 ok     = server_start("",
				      "(device (unit-id 1) (holding 0 (static 11)) (holding 1 (static 22)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_request(&client, 1, first, req, 5);
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 1, 1);
		ok = ok && send_request(&client, 1, second, req, 5);
	}
	if (ok)
		ok = recv_reply(&client, 1, first, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 11;
	if (ok)
		ok = recv_reply(&client, 1, second, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 22;

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

#if WITH_TLS
TEST(NetTlsMutualAuthServesMbapRead)
{
	tls_client_t	client = { .fd = -1, .ssl = NULL };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len  = 0;
	u16		tid  = ts_next_tid();
	bool		ok   = server_start(TLS_SERVER_OPTIONS,
					    "(device (unit-id 1) (holding 0 (static 4660)) "
					    "(holding 1 (static 7)))");

	ok = ok && tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				   TLS_CA_PATH, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 2);
		ok = tls_send_request(&client, 1, tid, req, 5) &&
		     tls_recv_reply(&client, 1, tid, resp, &len) &&
		     len == 6 &&
		     resp[0] == FC_READ_HOLDING_REGISTERS &&
		     register_value(resp, len, 0) == 4660 &&
		     register_value(resp, len, 1) == 7;
	}
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s\n", tls_client_error());

	tls_client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsGracefulShutdownSendsCloseNotify)
{
	tls_client_t	client = { .fd = -1, .ssl = NULL };
	bool		ok     = server_start(TLS_SERVER_OPTIONS,
					      "(device (unit-id 1) (holding 0 (static 4660)))");

	if (ok)
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				     TLS_CA_PATH, &client);
	server_stop();
	if (ok)
		ok = tls_client_wait_close_notify(&client, 2000);
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s\n", tls_client_error());

	tls_client_close(&client);
	ASSERT_EQ(ok);
}

TEST(NetTlsIdleTimeoutSendsCloseNotify)
{
	tls_client_t	client = { .fd = -1, .ssl = NULL };
	bool		ok     = server_start(TLS_IDLE_SERVER_OPTIONS,
					      "(device (unit-id 1) (holding 0 (static 4660)))");

	if (ok)
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				     TLS_CA_PATH, &client);
	if (ok)
		ok = tls_client_wait_close_notify(&client, 2000);
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s\n", tls_client_error());

	tls_client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsLogsAuthenticatedPeerSubject)
{
	tls_client_t	client = { .fd = -1, .ssl = NULL };
	bool		ok     = server_start(TLS_SERVER_OPTIONS,
					      "(device (unit-id 1) (holding 0 (static 4660)))");

	if (ok)
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				     TLS_CA_PATH, &client);
	if (ok)
	{
		for (u32 i = 0; i < 100; i++)
		{
			if (ts_file_contains(g_server.stderr_path, "CN=mogbus test client"))
				break;
			usleep(20000);
		}
		ok = ts_file_contains(g_server.stderr_path, "CN=mogbus test client");
	}
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s\n", tls_client_error());

	tls_client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsRejectsPlaintextClient)
{
	client_t	client = { .fd = -1 };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len  = 0;
	u16		tid  = ts_next_tid();
	bool		ok   = server_start(TLS_SERVER_OPTIONS,
					    "(device (unit-id 1) (holding 0 (static 4660)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_request(&client, 1, tid, req, 5) &&
		     !recv_reply(&client, 1, tid, resp, &len);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsRejectsClientWithoutCertificate)
{
	tls_client_t	client = { .fd = -1, .ssl = NULL };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len      = 0;
	u16		tid      = ts_next_tid();
	bool		rejected = false;
	bool		ok       = server_start(TLS_SERVER_OPTIONS,
					       "(device (unit-id 1) (holding 0 (static 4660)))");

	/* OpenSSL may reject the anonymous client during the handshake (client-side
	 * failure), or complete the handshake and then close without serving MBAP. */
	if (ok)
	{
		rejected = !tls_client_open(g_server.probe_port, NULL, NULL, TLS_CA_PATH, &client);
		if (!rejected)
		{
			read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
			(void)tls_send_request(&client, 1, tid, req, 5);
			rejected = !tls_recv_reply(&client, 1, tid, resp, &len);
		}
	}
	ok = ok && rejected;

	tls_client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsPipelinedRequestsBeyondRxBuffer)
{
	tls_client_t	client = { .fd = -1, .ssl = NULL };
	u8		frames[TLS_PIPELINE_COUNT][TLS_PIPELINE_FRAME_LEN];
	u16		tids[TLS_PIPELINE_COUNT];
	u8		resp[MODBUS_MAX_PDU];
	u32		len  = 0;
	bool		ok   = server_start(TLS_SERVER_OPTIONS,
					    "(device (unit-id 1) (holding 0 (static 4660)))");

	if (ok)
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				     TLS_CA_PATH, &client);
	if (ok)
	{
		for (u32 i = 0; i < TLS_PIPELINE_COUNT; i++)
			tids[i] = ts_next_tid();
		tls_build_pipeline(frames, tids);
		ok = tls_client_write_record(&client, &frames[0][0], TLS_PIPELINE_BYTES);
	}
	if (ok)
	{
		for (u32 i = 0; i < TLS_PIPELINE_COUNT && ok; i++)
		{
			ok = tls_recv_reply(&client, 1, tids[i], resp, &len) &&
			     len == 4 && register_value(resp, len, 0) == 4660;
		}
	}
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s\n", tls_client_error());

	tls_client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsDelayedFaultHonoredWithBufferedRequests)
{
	tls_client_t	client  = { .fd = -1, .ssl = NULL };
	u8		frames[TLS_PIPELINE_COUNT][TLS_PIPELINE_FRAME_LEN];
	u8		extra_req[8];
	u16		tids[TLS_PIPELINE_COUNT];
	u16		extra_tid;
	u8		resp[MODBUS_MAX_PDU];
	u32		len     = 0;
	u64		start   = 0;
	u64		elapsed = 0;
	bool		ok      = server_start(TLS_SERVER_OPTIONS,
					       "(device (unit-id 1) (holding 0 (static 4660)) "
					       " (fault (kind delay) (delay-ms 400) (percent 100)))");

	if (ok)
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				     TLS_CA_PATH, &client);
	if (ok)
	{
		for (u32 i = 0; i < TLS_PIPELINE_COUNT; i++)
			tids[i] = ts_next_tid();
		extra_tid = ts_next_tid();
		tls_build_pipeline(frames, tids);
		start = clock_now_ms();
		ok = tls_client_write_record(&client, &frames[0][0], TLS_PIPELINE_BYTES);
		/* Let the server queue the delayed reply first, then make the socket
		 * readable again. That event must not send the reply before its due time. */
		if (ok)
			usleep(100000);
		if (ok)
		{
			read_pdu(extra_req, FC_READ_HOLDING_REGISTERS, 0, 1);
			ok = tls_send_request(&client, 1, extra_tid, extra_req, 5);
		}
	}
	if (ok)
	{
		ok = tls_recv_reply(&client, 1, tids[0], resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 4660;
		elapsed = clock_now_ms() - start;
		ok = ok && elapsed >= 200;
	}
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s (elapsed %llu ms)\n",
			tls_client_error(), (unsigned long long)elapsed);

	tls_client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsClientDisconnectDuringDelayedResponse)
{
	tls_client_t	client  = { .fd = -1, .ssl = NULL };
	tls_client_t	probe   = { .fd = -1, .ssl = NULL };
	tls_client_t	fillers[TLS_DISCONNECT_FILLERS];
	u32		opened  = 0;
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len     = 0;
	u16		tid     = ts_next_tid();
	u16		probe_tid;
	bool		ok      = server_start(TLS_SERVER_OPTIONS,
					       "(device (unit-id 1) (holding 0 (static 4660)) "
					       " (fault (kind delay) (delay-ms 300) (percent 100)))");

	for (u32 i = 0; i < TLS_DISCONNECT_FILLERS; i++)
		fillers[i] = (tls_client_t){ .fd = -1, .ssl = NULL };

	/* Occupy every slot but one before the delayed client connects. */
	for (u32 i = 0; ok && i < TLS_DISCONNECT_FILLERS; i++)
	{
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				     TLS_CA_PATH, &fillers[i]);
		if (ok)
			opened++;
	}
	if (ok)
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH, TLS_CLIENT_KEY_PATH,
				     TLS_CA_PATH, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = tls_send_request(&client, 1, tid, req, 5);
	}
	if (ok)
	{
		usleep(50000);
		tls_client_close(&client);
		/* Reconnect before the queued 300 ms reply becomes due, with a timeout
		 * shorter than the remaining delay. */
		usleep(50000);
	}
	if (ok)
		ok = tls_client_open_timeout(g_server.probe_port, TLS_CLIENT_CERT_PATH,
					     TLS_CLIENT_KEY_PATH, TLS_CA_PATH,
					     TLS_PROBE_TIMEOUT_MS, &probe);
	if (ok)
	{
		probe_tid = ts_next_tid();
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = tls_send_request(&probe, 1, probe_tid, req, 5) &&
		     tls_recv_reply(&probe, 1, probe_tid, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 4660;
	}
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s\n", tls_client_error());

	tls_client_close(&probe);
	tls_client_close(&client);
	for (u32 i = 0; i < opened; i++)
		tls_client_close(&fillers[i]);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTlsHandshakeTimeoutFreesSlots)
{
	client_t	fillers[TLS_HANDSHAKE_FILLERS];
	tls_client_t	client   = { .fd = -1, .ssl = NULL };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len      = 0;
	u16		tid      = ts_next_tid();
	u32		opened   = 0;
	bool		ok       = server_start(TLS_SHORT_HANDSHAKE_OPTIONS,
						"(device (unit-id 1) (holding 0 (static 4660)))");

	for (u32 i = 0; i < TLS_HANDSHAKE_FILLERS; i++)
		fillers[i].fd = -1;

	/* Raw sockets that never send a ClientHello occupy every server slot. */
	for (u32 i = 0; ok && i < TLS_HANDSHAKE_FILLERS; i++)
	{
		ok = client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &fillers[i]);
		if (ok)
			opened++;
	}

	for (u32 i = 0; ok && i < opened; i++)
		ok = wait_for_server_close(&fillers[i], 2000);

	if (ok)
		ok = tls_client_open(g_server.probe_port, TLS_CLIENT_CERT_PATH,
				     TLS_CLIENT_KEY_PATH, TLS_CA_PATH, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = tls_send_request(&client, 1, tid, req, 5) &&
		     tls_recv_reply(&client, 1, tid, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 4660;
	}
	if (!ok)
		fprintf(stderr, "net test: TLS client: %s\n", tls_client_error());

	tls_client_close(&client);
	for (u32 i = 0; i < opened; i++)
		client_close(&fillers[i]);
	server_stop();
	ASSERT_EQ(ok && opened == TLS_HANDSHAKE_FILLERS);
}
#endif

TEST(NetUnitIdRoutingAndGatewayException)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1) (holding 0 (static 4369)))\n"
				    "(device (unit-id 2) (holding 0 (static 8738)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 0x1111;
	}
	if (ok)
		ok = exchange(&client, 2, req, 5, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 0x2222;
	if (ok)
		ok = exchange(&client, 3, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_READ_HOLDING_REGISTERS, EXC_GATEWAY_TARGET_FAILED);

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetTcpUnitZeroIsNotBroadcast)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len  = 0;
	/* broadcast-execute is rejected in tcp mode, so unit 0 is just a unit id. */
	bool	 ok   = server_start("",
				     "(device (unit-id 1) (holding 0 (static 11)))\n"
				     "(device (unit-id 2) (holding 0 (static 22)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		write_pdu(req, 0, 42);
		ok = exchange(&client, 0, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_WRITE_SINGLE_REGISTER, EXC_GATEWAY_TARGET_FAILED);
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 11;
		ok = ok && exchange(&client, 2, req, 5, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 22;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetExceptionsOverWire)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 1) (input 0))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		req[0] = 0x2B; /* MEI, but missing payload */
		ok = exchange(&client, 1, req, 1, resp, &len) &&
		     exception_is(resp, len, 0x2B, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 0);
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_READ_HOLDING_REGISTERS, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 7, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_READ_HOLDING_REGISTERS, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		write_pdu(req, 0, 1);	/* input 0 exists, holding 0 does not */
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_WRITE_SINGLE_REGISTER, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		/* Diagnostics subfunction 0x0000 echoes the request; supported subfunctions work. */
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, 0x0000);
		put_be16(req + 3, 0xBEEF);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 5 &&
		     memcmp(resp, req, 5) == 0;
	}
	if (ok)
	{
		put_be16(req + 1, 0x0001);
		put_be16(req + 3, 0x0000);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 5 &&
		     memcmp(resp, req, 5) == 0;
	}
	if (ok)
	{
		put_be16(req + 1, 0x0002);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 5 &&
		     get_be16(resp + 1) == 0x0002;
	}
	if (ok)
	{
		put_be16(req + 1, 0x0013);
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_DIAGNOSTICS, EXC_ILLEGAL_FUNCTION);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetDiagnosticsListenOnlySilenceOverWire)
{
	client_t	client = { .fd = -1 };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	bool		ok  = server_start("", "(device (unit-id 1) (holding 0 (static 4660)))");

	ok = ok && client_open(g_server.probe_port, NET_SHORT_TIMEOUT_MS, &client);
	if (ok)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, 0x0004);
		put_be16(req + 3, 0x0000);
		ok = send_request(&client, 1, ts_next_tid(), req, 5) &&
		     recv_none(&client, NET_SHORT_TIMEOUT_MS);
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_request(&client, 1, ts_next_tid(), req, 5) &&
		     recv_none(&client, NET_SHORT_TIMEOUT_MS);
	}
	if (ok)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, 0x0001);
		put_be16(req + 3, 0xFF00);
		ok = send_request(&client, 1, ts_next_tid(), req, 5) &&
		     recv_none(&client, NET_SHORT_TIMEOUT_MS);
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 0x1234;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetBadMbapHeaderClosesConnection)
{
	bool ok = server_start("", "(device (unit-id 1) (holding 0))");

	for (u32 i = 0; i < 4 && ok; i++)
	{
		client_t	client = { .fd = -1 };
		u8		bad[MBAP_HEADER_SIZE];
		mbap_header_t	header = { .transaction = 1, .protocol = 0, .length = 2, .unit = 1 };
		char		byte = 0;
		ssize_t		got  = 0;

		if (i == 0)
			header.protocol = 1;
		else if (i == 1)
			header.length = 0;
		else if (i == 2)
			header.length = 1;
		else
			header.length = 255;
		mbap_encode(bad, &header);

		ok = client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
		if (!ok)
			break;

		ok = send_all(client.fd, bad, sizeof(bad));
		if (ok)
			got = recv(client.fd, &byte, 1, 0);
		ok = ok && got == 0;	/* server closed, no exception response */

		client_close(&client);
	}

	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetMbapLengthBoundariesOverWire)
{
	bool ok = server_start("", "(device (unit-id 1) (holding 0 (static 4660)))");

	for (u32 i = 0; i < 2 && ok; i++)
	{
		client_t	client = { .fd = -1 };
		u8		frame[MBAP_HEADER_SIZE + MODBUS_MAX_PDU];
		u8		resp[MODBUS_MAX_PDU];
		mbap_header_t	header = { .transaction = (u16)(10 + i), .protocol = 0, .unit = 1 };
		u32		pdu_len = 0;
		u32		frame_len = 0;

		if (i == 0)
		{
			/* length=2 means PDU is one byte: FC 07 is the smallest valid request. */
			header.length = 2;
			mbap_encode(frame, &header);
			frame[MBAP_HEADER_SIZE] = FC_READ_EXCEPTION_STATUS;
			pdu_len = 1;
			frame_len = MBAP_HEADER_SIZE + pdu_len;
		}
		else
		{
			/* length=254 means PDU is 253 bytes: max-size FC 03 read request. */
			header.length = MODBUS_MAX_PDU + 1;
			mbap_encode(frame, &header);
			frame[MBAP_HEADER_SIZE + 0] = FC_READ_HOLDING_REGISTERS;
			put_be16(frame + MBAP_HEADER_SIZE + 1, 0);
			put_be16(frame + MBAP_HEADER_SIZE + 3, 125);
			memset(frame + MBAP_HEADER_SIZE + 5, 0, MODBUS_MAX_PDU - 5);
			pdu_len = MODBUS_MAX_PDU;
			frame_len = MBAP_HEADER_SIZE + pdu_len;
		}

		ok = client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
		if (ok)
			ok = send_all(client.fd, frame, frame_len);
		if (ok)
		{
			u32 got = 0;

			ok = recv_reply(&client, 1, header.transaction, resp, &got);
			if (ok && i == 0)
				ok = got == 2 && resp[0] == FC_READ_EXCEPTION_STATUS && resp[1] == 0;
			if (ok && i == 1)
				ok = got == 2 && resp[0] == (u8)(FC_READ_HOLDING_REGISTERS | 0x80) &&
				     resp[1] == EXC_ILLEGAL_DATA_VALUE;
		}
		client_close(&client);
	}

	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetFragmentedAndCoalescedRequests)
{
	client_t	client = { .fd = -1 };
	u8		req1[MBAP_HEADER_SIZE + 5];
	u8		req2[MBAP_HEADER_SIZE + 5];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	u16		tid1 = ts_next_tid();
	u16		tid2 = ts_next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (static 4369)) (holding 1 (static 8738)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		mbap_header_t	h = { .transaction = tid1, .length = 6, .unit = 1 };

		mbap_encode(req1, &h);
		read_pdu(req1 + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		for (u32 i = 0; i < sizeof(req1) && ok; i++)
		{
			ok = send_all(client.fd, req1 + i, 1);
			if (ok && i == 2)
				usleep(2000);	/* split inside MBAP header */
		}
	}
	if (ok)
		ok = recv_reply(&client, 1, tid1, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 0x1111;

	if (ok)
	{
		mbap_header_t	h1 = { .transaction = tid1, .length = 6, .unit = 1 };
		mbap_header_t	h2 = { .transaction = tid2, .length = 6, .unit = 1 };
		mbap_header_t	h3 = { .transaction = (u16)(tid2 + 1), .length = 6, .unit = 1 };
		u8		buf[(MBAP_HEADER_SIZE + 5) * 2 + 3];
		u8		req3[MBAP_HEADER_SIZE + 5];

		mbap_encode(req1, &h1);
		read_pdu(req1 + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		mbap_encode(req2, &h2);
		read_pdu(req2 + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 1, 1);
		mbap_encode(req3, &h3);
		read_pdu(req3 + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		memcpy(buf, req1, sizeof(req1));
		memcpy(buf + sizeof(req1), req2, sizeof(req2));
		memcpy(buf + sizeof(req1) + sizeof(req2), req3, 3);
		/* Two full requests plus the first 3 bytes of a third request in one segment. */
		ok = send_all(client.fd, buf, sizeof(buf));
	}
	if (ok)
		ok = recv_reply(&client, 1, tid1, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 0x1111;
	if (ok)
		ok = recv_reply(&client, 1, tid2, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 0x2222;
	if (ok)
	{
		/* Complete the third request whose header was already sent. */
		u8		tail[MBAP_HEADER_SIZE + 5 - 3];
		mbap_header_t	h3 = { .transaction = (u16)(tid2 + 1), .length = 6, .unit = 1 };
		u8		req3[MBAP_HEADER_SIZE + 5];

		mbap_encode(req3, &h3);
		read_pdu(req3 + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		memcpy(tail, req3 + 3, sizeof(tail));
		ok = send_all(client.fd, tail, sizeof(tail));
		if (ok)
			ok = recv_reply(&client, 1, h3.transaction, resp, &len) && len == 4 &&
			     register_value(resp, len, 0) == 0x1111;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetMaxSizePduBothDirections)
{
	client_t	client = { .fd = -1 };
	u8		req[MBAP_HEADER_SIZE + MODBUS_MAX_PDU];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	bool		ok = server_start("",
					 "(device (unit-id 1)"
					 "  (coil 0 (count 2000) (static on))"
					 "  (holding 0 (count 125) (static 4660)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	/* FC 01: 2000 bits -> response PDU is 252 bytes (max read bits). */
	if (ok)
	{
		mbap_header_t	h = { .transaction = ts_next_tid(), .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_COILS, 0, 2000);
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 5) &&
		     recv_reply(&client, 1, h.transaction, resp, &len);
		ok = ok && len == 252 && resp[0] == FC_READ_COILS && resp[1] == 250;
		for (u32 i = 0; ok && i < 250; i++)
			ok = resp[2 + i] == 0xFF;
	}

	/* FC 03: 125 registers -> response PDU is 252 bytes (max read registers). */
	if (ok)
	{
		mbap_header_t	h = { .transaction = ts_next_tid(), .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 125);
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 5) &&
		     recv_reply(&client, 1, h.transaction, resp, &len);
		ok = ok && len == 252 && resp[0] == FC_READ_HOLDING_REGISTERS &&
		     resp[1] == 250;
		for (u32 i = 0; ok && i < 125; i++)
			ok = get_be16(resp + 2 + i * 2) == 4660;
	}

	/* FC 16: 123 registers -> request PDU is 252 bytes (max write registers). */
	if (ok)
	{
		mbap_header_t	h = { .transaction = ts_next_tid(), .length = 253, .unit = 1 };

		mbap_encode(req, &h);
		req[MBAP_HEADER_SIZE + 0] = FC_WRITE_MULTIPLE_REGISTERS;
		put_be16(req + MBAP_HEADER_SIZE + 1, 0);
		put_be16(req + MBAP_HEADER_SIZE + 3, 123);
		req[MBAP_HEADER_SIZE + 5] = 246;
		for (u32 i = 0; i < 123; i++)
			put_be16(req + MBAP_HEADER_SIZE + 6 + i * 2, (u16)(0x2000 + i));
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 252) &&
		     recv_reply(&client, 1, h.transaction, resp, &len);
		ok = ok && len == 5 && resp[0] == FC_WRITE_MULTIPLE_REGISTERS &&
		     get_be16(resp + 3) == 123;
	}
	if (ok)
	{
		mbap_header_t	h = { .transaction = ts_next_tid(), .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 122, 1);
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 5) &&
		     recv_reply(&client, 1, h.transaction, resp, &len);
		ok = ok && len == 4 && register_value(resp, len, 0) == 0x2000 + 122;
	}

	/* FC 15: 1968 bits -> request PDU is 252 bytes (max write bits). */
	if (ok)
	{
		mbap_header_t	h = { .transaction = ts_next_tid(), .length = 253, .unit = 1 };

		mbap_encode(req, &h);
		req[MBAP_HEADER_SIZE + 0] = FC_WRITE_MULTIPLE_COILS;
		put_be16(req + MBAP_HEADER_SIZE + 1, 0);
		put_be16(req + MBAP_HEADER_SIZE + 3, 1968);
		req[MBAP_HEADER_SIZE + 5] = 246;
		memset(req + MBAP_HEADER_SIZE + 6, 0xAA, 246);
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 252) &&
		     recv_reply(&client, 1, h.transaction, resp, &len);
		ok = ok && len == 5 && resp[0] == FC_WRITE_MULTIPLE_COILS &&
		     get_be16(resp + 3) == 1968;
	}
	if (ok)
	{
		mbap_header_t	h = { .transaction = ts_next_tid(), .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_COILS, 0, 8);
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 5) &&
		     recv_reply(&client, 1, h.transaction, resp, &len);
		ok = ok && len == 3 && resp[2] == 0xAA;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetPartialSendLargeResponse)
{
	client_t	client = { .fd = -1 };
	u8		req[MBAP_HEADER_SIZE + 5];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	u16		tid = ts_next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (count 125) (static 13124)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
		ok = set_small_send_buffer(client.fd, 1024);
	if (ok)
	{
		mbap_header_t	h = { .transaction = tid, .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 125);
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 5);
	}
	if (ok)
		ok = recv_reply(&client, 1, tid, resp, &len);
	ASSERT_EQ(ok);
	ASSERT_EQ(len == 252);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == 250);
	for (u32 i = 0; i < 125; i++)
		ASSERT_EQ(get_be16(resp + 2 + i * 2) == 13124);

	client_close(&client);
	server_stop();
}

TEST(NetClientDisconnectDuringDelayedResponse)
{
	client_t	client = { .fd = -1 };
	client_t	probe = { .fd = -1 };
	u8		req[MBAP_HEADER_SIZE + 5];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	u16		tid = ts_next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (static 9))"
					 "  (fault (kind delay) (delay-ms 300) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		mbap_header_t	h = { .transaction = tid, .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_all(client.fd, req, sizeof(req));
	}
	if (ok)
	{
		usleep(50000);
		client_close(&client);	/* disconnect while response is delayed/pending */
		usleep(400000);
	}

	/* Server should remain healthy and serve other clients. */
	if (ok)
		ok = client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &probe);
	if (ok)
	{
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&probe, 1, req + MBAP_HEADER_SIZE, 5, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 9;
	}

	client_close(&probe);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetClientHalfCloseDuringDelayedResponse)
{
	client_t	client = { .fd = -1 };
	client_t	probe = { .fd = -1 };
	u8		req[MBAP_HEADER_SIZE + 5];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	u16		tid = ts_next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (static 9))"
					 "  (fault (kind delay) (delay-ms 300) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		mbap_header_t	h = { .transaction = tid, .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_all(client.fd, req, sizeof(req));
	}
	if (ok)
	{
		usleep(50000);
		ok = shutdown(client.fd, SHUT_WR) == 0;	/* half-close: client done sending */
	}
	if (ok)
		ok = recv_reply(&client, 1, tid, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 9;

	/* After response delivery, server should close and remain healthy. */
	if (ok)
		ok = wait_for_server_close(&client, 1500);
	if (ok)
		ok = client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &probe);
	if (ok)
	{
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&probe, 1, req + MBAP_HEADER_SIZE, 5, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 9;
	}

	client_close(&probe);
	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetDelayFaultDoesNotBlockOtherClients)
{
	client_t	delayed   = { .fd = -1 };
	client_t	fast      = { .fd = -1 };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len       = 0;
	u16		delayed_tid = 0;
	u16		fast_tid  = 0;
	u64		start     = 0;
	u64		fast_ms   = 0;
	u64		delayed_ms = 0;
	bool		ok        = server_start("",
						"(device (unit-id 1) (holding 0 (static 11))\n"
						"  (fault (kind delay) (delay-ms 600) (percent 100)))\n"
						"(device (unit-id 2) (holding 0 (static 22)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &delayed);
	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &fast);

	if (ok)
	{
		delayed_tid = ts_next_tid();
		fast_tid    = ts_next_tid();
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		start = clock_now_ms();
		ok = send_request(&delayed, 1, delayed_tid, req, 5);
		ok = ok && send_request(&fast, 2, fast_tid, req, 5);
	}
	if (ok)
	{
		ok = recv_reply(&fast, 2, fast_tid, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 22;
		fast_ms = clock_now_ms() - start;
	}
	if (ok)
	{
		ok = recv_reply(&delayed, 1, delayed_tid, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 11;
		delayed_ms = clock_now_ms() - start;
	}
	ok = ok && fast_ms < 300 && delayed_ms >= 500;

	client_close(&delayed);
	client_close(&fast);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetDropFaultStillAppliesWrites)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len     = 0;
	bool	 dropped = false;
	bool	 ok      = server_start("(seed 1)",
					"(device (unit-id 1) (holding 0 (static 0))\n"
					"  (fault (kind drop) (percent 50)))");

	ok = ok && client_open(g_server.probe_port, NET_SHORT_TIMEOUT_MS, &client);

	/* A dropped request makes exchange() time out. Repeat until one is dropped. */
	for (u32 i = 0; ok && i < NET_MAX_ATTEMPTS && !dropped; i++)
	{
		write_pdu(req, 0, 1234);
		dropped = !exchange(&client, 1, req, 5, resp, &len);
	}

	/* Read until a response gets through, then check the write took effect. */
	for (u32 i = 0; ok && dropped && i < NET_MAX_ATTEMPTS; i++)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		if (exchange(&client, 1, req, 5, resp, &len) && len == 4)
		{
			ok = register_value(resp, len, 0) == 1234;
			break;
		}
	}
	ok = ok && dropped;

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetExceptionFaultReachesClient)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len  = 0;
	bool	 ok   = server_start("(seed 1)",
				     "(device (unit-id 1) (holding 0 (static 42))"
				     "  (fault (kind exception) (code 6) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_READ_HOLDING_REGISTERS, EXC_SLAVE_DEVICE_BUSY);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetCorruptFaultReachesClient)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len  = 0;
	bool	 ok   = server_start("(seed 2)",
				     "(device (unit-id 1) (holding 0 (static 42))"
				     "  (fault (kind corrupt) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     len == 4 &&
		     resp[0] == FC_READ_HOLDING_REGISTERS &&
		     resp[1] == (0x02 ^ 0xFF) &&
		     register_value(resp, len, 0) == 42;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetDelayAndCorruptFaultsCombine)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len  = 0;
	u64	 start;
	bool	 ok = server_start("(seed 3)",
				   "(device (unit-id 1) (holding 0 (static 42))"
				   "  (fault (kind delay) (delay-ms 300) (percent 100))"
				   "  (fault (kind corrupt) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		start = clock_now_ms();
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     clock_now_ms() - start >= 300 &&
		     len == 4 &&
		     resp[0] == FC_READ_HOLDING_REGISTERS &&
		     resp[1] == (0x02 ^ 0xFF) &&
		     register_value(resp, len, 0) == 42;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetExceptionAndCorruptFaultsCombine)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len  = 0;
	bool	 ok   = server_start("(seed 3)",
				     "(device (unit-id 1) (holding 0 (static 42))"
				     "  (fault (kind exception) (code 6) (percent 100))"
				     "  (fault (kind corrupt) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     len == 2 &&
		     resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80) &&
		     resp[1] == (EXC_SLAVE_DEVICE_BUSY ^ 0xFF);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetDropAndExceptionFaultsSuppressResponse)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	bool	 ok   = server_start("(seed 3)",
				     "(device (unit-id 1) (holding 0 (static 42))"
				     "  (fault (kind drop) (percent 100))"
				     "  (fault (kind exception) (code 6) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_request(&client, 1, ts_next_tid(), req, 5) &&
		     ts_no_data(client.fd, 200);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetDisconnectFaultClosesClient)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len  = 0;
	bool	 ok   = server_start("(seed 3)",
				     "(device (unit-id 1) (holding 0 (static 42))"
				     "  (fault (kind disconnect) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_request(&client, 1, ts_next_tid(), req, 5) &&
		     wait_for_server_close(&client, 2000) &&
		     !recv_reply(&client, 1, 0, resp, &len);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetConnectionLimit)
{
	client_t clients[32];
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len   = 0;
	bool	 ok    = server_start("", "(device (unit-id 1) (holding 0 (static 5)))");
	u32	 opened = 0;

	for (u32 i = 0; i < 32; i++)
	{
		clients[i].fd = -1;
		ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &clients[i]);
		if (!ok)
			break;
		opened++;
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&clients[0], 1, req, 5, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 5;
	}

	/* The 33rd connection is either refused outright, closed by the server,
	 * or left unserviced in the TCP backlog under load. A served Modbus response
	 * would mean the server exceeded its connection limit. */
	if (ok)
	{
		client_t extra = { .fd = -1 };
		bool	 open  = client_open(g_server.probe_port, NET_SHORT_TIMEOUT_MS, &extra);

		if (open)
		{
			u8  extra_req[8];
			u8  extra_resp[MODBUS_MAX_PDU];
			u32 extra_len = 0;

			read_pdu(extra_req, FC_READ_HOLDING_REGISTERS, 0, 1);
			ok = !exchange(&extra, 1, extra_req, 5, extra_resp, &extra_len);
			client_close(&extra);
		}
	}

	for (u32 i = 0; i < opened; i++)
		client_close(&clients[i]);
	server_stop();
	ASSERT_EQ(ok && opened == 32);
}

TEST(NetWritesAreAllOrNothing)
{
	client_t client = { .fd = -1 };
	u8	 req[16];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)\n"
				    "  (holding 0 (static 1)) (holding 1 (static 2)) (holding 2 (static 3))\n"
				    "  (coil 0 (static off)) (coil 1 (static off)) (coil 2 (static off))\n"
				    "  (input 0 (static 9)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	/* Write multiple registers, then read them back. */
	if (ok)
	{
		req[0] = FC_WRITE_MULTIPLE_REGISTERS;
		put_be16(req + 1, 0);
		put_be16(req + 3, 3);
		req[5] = 6;
		put_be16(req + 6, 100);
		put_be16(req + 8, 200);
		put_be16(req + 10, 300);
		ok = exchange(&client, 1, req, 12, resp, &len) && len == 5 && resp[0] == req[0];
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 3);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 8 &&
		     register_value(resp, len, 0) == 100 && register_value(resp, len, 1) == 200 &&
		     register_value(resp, len, 2) == 300;
	}

	/* One undefined address makes the whole write fail, leaving the others untouched. */
	if (ok)
	{
		req[0] = FC_WRITE_MULTIPLE_REGISTERS;
		put_be16(req + 1, 1);
		put_be16(req + 3, 3);
		req[5] = 6;
		put_be16(req + 6, 400);
		put_be16(req + 8, 500);
		put_be16(req + 10, 600);
		ok = exchange(&client, 1, req, 12, resp, &len) &&
		     exception_is(resp, len, FC_WRITE_MULTIPLE_REGISTERS, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 1, 2);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 6 &&
		     register_value(resp, len, 0) == 200 && register_value(resp, len, 1) == 300;
	}

	/* Same rule for coils: a single-coil write succeeds, a multi-coil write with an
	 * undefined address changes nothing. */
	if (ok)
	{
		req[0] = FC_WRITE_SINGLE_COIL;
		put_be16(req + 1, 0);
		req[3] = 0xFF;
		req[4] = 0x00;
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 5 && resp[0] == req[0];
	}
	if (ok)
	{
		req[0] = FC_WRITE_MULTIPLE_COILS;
		put_be16(req + 1, 0);
		put_be16(req + 3, 4);
		req[5] = 1;
		req[6] = 0x0F;
		ok = exchange(&client, 1, req, 7, resp, &len) &&
		     exception_is(resp, len, FC_WRITE_MULTIPLE_COILS, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		read_pdu(req, FC_READ_COILS, 0, 3);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 3 && resp[2] == 1;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetGetComEventCounterReturnsStatusAndCount)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4;
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[0] == FC_GET_COM_EVENT_COUNTER && resp[1] == 0x00 && resp[2] == 0x00 &&
		     resp[3] == 0x00 && resp[4] == 0x01;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetGetComEventCounterMalformedRequest)
{
	client_t client = { .fd = -1 };
	u8	 req[8] = { FC_GET_COM_EVENT_COUNTER, 0x00 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, 2, resp, &len) &&
		     exception_is(resp, len, FC_GET_COM_EVENT_COUNTER, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x00;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetGetComEventCounterUnknownUnitDoesNotCount)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		ok = exchange(&client, 3, req, 1, resp, &len) &&
		     exception_is(resp, len, FC_GET_COM_EVENT_COUNTER, EXC_GATEWAY_TARGET_FAILED);
	}
	if (ok)
	{
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x00;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetGetComEventLogReturnsCountsAndEmptyLog)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4;
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_LOG;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 8 &&
		     resp[0] == FC_GET_COM_EVENT_LOG && resp[1] == 6 &&
		     resp[2] == 0x00 && resp[3] == 0x00 &&
		     resp[4] == 0x00 && resp[5] == 0x01 &&
		     resp[6] == 0x00 && resp[7] == 0x02;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetGetComEventLogMatchesComEventCounter)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4;
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x01;
	}
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 0); /* exception response */
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_READ_HOLDING_REGISTERS, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_LOG;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 8 &&
		     resp[1] == 6 && resp[4] == 0x00 && resp[5] == 0x01 &&
		     resp[6] == 0x00 && resp[7] == 0x04;
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x01;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetGetComEventLogMalformedRequest)
{
	client_t client = { .fd = -1 };
	u8	 req[8] = { FC_GET_COM_EVENT_LOG, 0x00 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, 2, resp, &len) &&
		     exception_is(resp, len, FC_GET_COM_EVENT_LOG, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_LOG;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 8 &&
		     resp[1] == 6 && resp[4] == 0x00 && resp[5] == 0x00 &&
		     resp[6] == 0x00 && resp[7] == 0x02;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetGetComEventLogUnknownUnitDoesNotCount)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_LOG;
		ok = exchange(&client, 3, req, 1, resp, &len) &&
		     exception_is(resp, len, FC_GET_COM_EVENT_LOG, EXC_GATEWAY_TARGET_FAILED);
	}
	if (ok)
	{
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 8 &&
		     resp[1] == 6 && resp[4] == 0x00 && resp[5] == 0x00 &&
		     resp[6] == 0x00 && resp[7] == 0x01;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetComStatusAndEventsAreReported)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (com-status 65535) (com-event 1) (com-event 128) "
						   "(com-event 255) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[0] == FC_GET_COM_EVENT_COUNTER && resp[1] == 0xFF && resp[2] == 0xFF &&
		     resp[3] == 0x00 && resp[4] == 0x00;
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_LOG;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 11 &&
		     resp[0] == FC_GET_COM_EVENT_LOG && resp[1] == 9 &&
		     resp[2] == 0xFF && resp[3] == 0xFF &&
		     resp[4] == 0x00 && resp[5] == 0x00 &&
		     resp[6] == 0x00 && resp[7] == 0x02 &&
		     resp[8] == 0x01 && resp[9] == 0x80 && resp[10] == 0xFF;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetComEventLogMaxEventsAreReported)
{
	client_t client   = { .fd = -1 };
	char	 devices[1024];
	u32	 used     = 0;
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len      = 0;
	bool	 ok;

	used = (u32)snprintf(devices, sizeof(devices), "(device (unit-id 1) (holding 0)");
	for (u32 i = 0; i < DEVICE_MAX_COM_EVENTS; i++)
		used += (u32)snprintf(devices + used, sizeof(devices) - used, " (com-event %u)", i);
	snprintf(devices + used, sizeof(devices) - used, ")");

	ok = server_start("", devices);
	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_LOG;
		ok = exchange(&client, 1, req, 1, resp, &len) &&
		     len == 8 + DEVICE_MAX_COM_EVENTS &&
		     resp[0] == FC_GET_COM_EVENT_LOG && resp[1] == 6 + DEVICE_MAX_COM_EVENTS &&
		     resp[2] == 0x00 && resp[3] == 0x00 &&
		     resp[4] == 0x00 && resp[5] == 0x00 &&
		     resp[6] == 0x00 && resp[7] == 0x01;
		for (u32 i = 0; ok && i < DEVICE_MAX_COM_EVENTS; i++)
			ok = resp[8 + i] == (u8)i;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetComStatusAndEventsRejectMalformedRequests)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (com-status 65535) (com-event 66) (holding 0))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		req[1] = 0x00;
		ok = exchange(&client, 1, req, 2, resp, &len) &&
		     exception_is(resp, len, FC_GET_COM_EVENT_COUNTER, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_LOG;
		ok = exchange(&client, 1, req, 2, resp, &len) &&
		     exception_is(resp, len, FC_GET_COM_EVENT_LOG, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 9 &&
		     resp[1] == 7 && resp[2] == 0xFF && resp[3] == 0xFF &&
		     resp[4] == 0x00 && resp[5] == 0x00 &&
		     resp[6] == 0x00 && resp[7] == 0x03 && resp[8] == 0x42;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReportServerIdReturnsConfiguredValues)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (server-id 42) (run-indicator 0) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		req[0] = FC_REPORT_SERVER_ID;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 4 &&
		     resp[0] == FC_REPORT_SERVER_ID && resp[1] == 2 &&
		     resp[2] == 42 && resp[3] == 0x00;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReportServerIdDefaults)
{
	client_t client = { .fd = -1 };
	u8	 req[8] = { FC_REPORT_SERVER_ID };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 4 &&
		     resp[0] == FC_REPORT_SERVER_ID && resp[1] == 2 &&
		     resp[2] == 0x00 && resp[3] == 0xFF;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReportServerIdReturnsConfiguredAdditionalData)
{
	client_t	client = { .fd = -1 };
	u8		req[8] = { FC_REPORT_SERVER_ID };
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	bool		ok  = server_start("", "(device (unit-id 1) (server-id 42) (run-indicator 0) "
					   "(server-id-additional-data \"DEADBEEF\") (holding 0 (static 1)))");
	const u8	extra[] = { 0xDE, 0xAD, 0xBE, 0xEF };

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 8 &&
		     resp[0] == FC_REPORT_SERVER_ID && resp[1] == 6 &&
		     resp[2] == 42 && resp[3] == 0x00 &&
		     memcmp(resp + 4, extra, sizeof(extra)) == 0;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReportServerIdMalformedRequest)
{
	client_t client = { .fd = -1 };
	u8	 req[8] = { FC_REPORT_SERVER_ID, 0x00 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, 2, resp, &len) &&
		     exception_is(resp, len, FC_REPORT_SERVER_ID, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		req[0] = FC_REPORT_SERVER_ID;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 4 &&
		     resp[1] == 2 && resp[2] == 0x00 && resp[3] == 0xFF;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReportServerIdUnknownUnitDoesNotCount)
{
	client_t client = { .fd = -1 };
	u8	 req[8] = { FC_REPORT_SERVER_ID };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 3, req, 1, resp, &len) &&
		     exception_is(resp, len, FC_REPORT_SERVER_ID, EXC_GATEWAY_TARGET_FAILED);
	}
	if (ok)
	{
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 4 &&
		     resp[1] == 2 && resp[2] == 0x00 && resp[3] == 0xFF;
	}
	if (ok)
	{
		req[0] = FC_GET_COM_EVENT_COUNTER;
		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x01;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadFileRecordSuccessAndErrors)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (file 1 (record 0 4660 22136) (record 10 7))"
				    "  (file 4 (record 2 255)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		/* Single sub-request: file 1, record 0, length 2. */
		u8 pdu[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02 };

		ok = exchange(&client, 1, pdu, sizeof(pdu), resp, &len) && len == 8 &&
		     resp[0] == FC_READ_FILE_RECORD && resp[1] == 6 &&
		     resp[2] == 5 && resp[3] == 0x06 &&
		     get_be16(resp + 4) == 0x1234 && get_be16(resp + 6) == 0x5678;
	}
	if (ok)
	{
		/* Multiple sub-requests. */
		u8 pdu[] = {
			FC_READ_FILE_RECORD, 0x0E,
			0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
			0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01,
		};

		ok = exchange(&client, 1, pdu, sizeof(pdu), resp, &len) && len == 10 &&
		     resp[0] == FC_READ_FILE_RECORD && resp[1] == 8 &&
		     resp[2] == 3 && resp[3] == 0x06 && get_be16(resp + 4) == 0x5678 &&
		     resp[6] == 3 && resp[7] == 0x06 && get_be16(resp + 8) == 0x00FF;
	}
	if (ok)
	{
		/* Malformed: bad byte count framing. */
		u8 pdu[] = { FC_READ_FILE_RECORD, 0x06, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };

		ok = exchange(&client, 1, pdu, sizeof(pdu), resp, &len) &&
		     exception_is(resp, len, FC_READ_FILE_RECORD, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		/* Unavailable file. */
		u8 pdu[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01 };

		ok = exchange(&client, 1, pdu, sizeof(pdu), resp, &len) &&
		     exception_is(resp, len, FC_READ_FILE_RECORD, EXC_ILLEGAL_DATA_ADDRESS);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadFileRecordUnknownUnit)
{
	client_t client = { .fd = -1 };
	u8	 req[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (file 1 (record 0 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 3, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_FILE_RECORD, EXC_GATEWAY_TARGET_FAILED);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadFileRecordUnavailableWhenNoFileRecordsConfigured)
{
	client_t client = { .fd = -1 };
	u8	 req[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_FILE_RECORD, EXC_ILLEGAL_DATA_ADDRESS);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetWriteFileRecordSuccessAndReadBack)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (file 1 (record 0 4660 22136) (record 10 7)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		u8 req[] = { FC_WRITE_FILE_RECORD, 0x0B, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xAB, 0xCD, 0x00, 0x99 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == sizeof(req) &&
		     memcmp(resp, req, sizeof(req)) == 0;
	}
	if (ok)
	{
		u8 read[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02 };

		ok = exchange(&client, 1, read, sizeof(read), resp, &len) && len == 8 &&
		     resp[0] == FC_READ_FILE_RECORD && resp[1] == 6 &&
		     get_be16(resp + 4) == 0xABCD && get_be16(resp + 6) == 0x0099;
	}
	if (ok)
	{
		/* Multiple sub-requests. */
		u8 req[] = {
			FC_WRITE_FILE_RECORD, 0x12,
			0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x55,
			0x06, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x66,
		};

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == sizeof(req) &&
		     memcmp(resp, req, sizeof(req)) == 0;
	}
	if (ok)
	{
		u8 read[] = {
			FC_READ_FILE_RECORD, 0x0E,
			0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
			0x06, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x01,
		};

		ok = exchange(&client, 1, read, sizeof(read), resp, &len) && len == 10 &&
		     get_be16(resp + 4) == 0x0055 && get_be16(resp + 8) == 0x0066;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetWriteFileRecordErrorsAndAllOrNothing)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (file 1 (record 0 4660 22136)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		/* Malformed: byte count/framing mismatch. */
		u8 bad[] = { FC_WRITE_FILE_RECORD, 0x08, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 };

		ok = exchange(&client, 1, bad, sizeof(bad), resp, &len) &&
		     exception_is(resp, len, FC_WRITE_FILE_RECORD, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		u8 missing[] = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 };

		ok = exchange(&client, 1, missing, sizeof(missing), resp, &len) &&
		     exception_is(resp, len, FC_WRITE_FILE_RECORD, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		/* All-or-nothing: first sub-request valid, second unavailable. */
		u8 req[] = {
			FC_WRITE_FILE_RECORD, 0x12,
			0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x77,
			0x06, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x88,
		};

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_WRITE_FILE_RECORD, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		u8 read[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };

		ok = exchange(&client, 1, read, sizeof(read), resp, &len) && len == 6 &&
		     get_be16(resp + 4) == 0x1234;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetWriteFileRecordUnknownUnitAndUnconfigured)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (file 1 (record 0 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		u8 req[] = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02 };

		ok = exchange(&client, 3, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_WRITE_FILE_RECORD, EXC_GATEWAY_TARGET_FAILED);
	}
	client_close(&client);
	server_stop();

	if (ok)
	{
		ok = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");
		client.fd = -1;
		ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	}
	if (ok)
	{
		u8 req[] = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_WRITE_FILE_RECORD, EXC_ILLEGAL_DATA_ADDRESS);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetMaskWriteRegisterSuccessMalformedAndUnknownUnit)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (holding 0 (static 4660))"
				    "  (input 0 (static 7)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		/* holding[0] starts at 4660 (0x1234): (0x1234 & 0xF202) | (0x0025 & ~0xF202) = 0x1225 */
		u8 req[] = { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0xF2, 0x02, 0x00, 0x25 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == sizeof(req) &&
		     memcmp(resp, req, sizeof(req)) == 0;
	}
	if (ok)
	{
		u8 req[8];

		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 0x1225;
	}
	if (ok)
	{
		u8 short_req[] = { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0xFF, 0xFF, 0x00 };

		ok = exchange(&client, 1, short_req, sizeof(short_req), resp, &len) &&
		     exception_is(resp, len, FC_MASK_WRITE_REGISTER, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		u8 missing[] = { FC_MASK_WRITE_REGISTER, 0x00, 0x05, 0xFF, 0xFF, 0x00, 0x00 };

		ok = exchange(&client, 1, missing, sizeof(missing), resp, &len) &&
		     exception_is(resp, len, FC_MASK_WRITE_REGISTER, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		u8 req[] = { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };

		ok = exchange(&client, 3, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_MASK_WRITE_REGISTER, EXC_GATEWAY_TARGET_FAILED);
	}
	if (ok)
	{
		u8 req[] = { FC_GET_COM_EVENT_COUNTER };

		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x02;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadWriteMultipleRegistersSuccessAndOverlap)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (holding 0 (static 4660))"
				    "  (holding 1 (static 22136))"
				    "  (holding 2 (static 3))"
				    "  (input 0 (static 7)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		/* Read 0,1 while writing 0,1 -> response reflects post-write values. */
		u8 req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x04, 0xAA, 0xAA, 0xBB, 0xBB };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == 6 &&
		     resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS && resp[1] == 0x04 &&
		     register_value(resp, len, 0) == 0xAAAA && register_value(resp, len, 1) == 0xBBBB;
	}
	if (ok)
	{
		u8 req[8];

		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 2);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 6 &&
		     register_value(resp, len, 0) == 0xAAAA && register_value(resp, len, 1) == 0xBBBB;
	}
	if (ok)
	{
		/* Overlap: read 1,2 while writing 0,1 -> read sees new 1 and old 2. */
		u8 req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x64, 0x00, 0x65 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == 6 &&
		     resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS && resp[1] == 0x04 &&
		     register_value(resp, len, 0) == 0x0065 && register_value(resp, len, 1) == 0x0003;
	}
	if (ok)
	{
		u8 req[8];

		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 3);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 8 &&
		     register_value(resp, len, 0) == 0x0064 && register_value(resp, len, 1) == 0x0065 &&
		     register_value(resp, len, 2) == 0x0003;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadWriteMultipleRegistersErrorsAndAllOrNothing)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (holding 0 (static 10))"
				    "  (holding 1 (static 20))"
				    "  (input 0 (static 7)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		u8 bad_qty[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x01 };

		ok = exchange(&client, 1, bad_qty, sizeof(bad_qty), resp, &len) &&
		     exception_is(resp, len, FC_READ_WRITE_MULTIPLE_REGISTERS, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		u8 bad_bc[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0x00, 0x00 };

		ok = exchange(&client, 1, bad_bc, sizeof(bad_bc), resp, &len) &&
		     exception_is(resp, len, FC_READ_WRITE_MULTIPLE_REGISTERS, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		/* Read range unavailable (holding 2 undefined): must not apply write to holding 0. */
		u8 req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x63 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_WRITE_MULTIPLE_REGISTERS, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		u8 req[8];

		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     register_value(resp, len, 0) == 10;
	}
	if (ok)
	{
		/* Write range unavailable (holding 2 undefined): must not apply any write. */
		u8 req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x64, 0x00, 0x65 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_WRITE_MULTIPLE_REGISTERS, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		u8 req[8];

		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 2);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 6 &&
		     register_value(resp, len, 0) == 10 && register_value(resp, len, 1) == 20;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadWriteMultipleRegistersUnknownUnitAndNonHoldingTarget)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (holding 1 (static 10))"
				    "  (input 0 (static 7)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		u8 req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x01 };

		ok = exchange(&client, 3, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_WRITE_MULTIPLE_REGISTERS, EXC_GATEWAY_TARGET_FAILED);
	}
	if (ok)
	{
		/* input register 0 exists but holding target does not */
		u8 req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x01 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_WRITE_MULTIPLE_REGISTERS, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		u8 req[] = { FC_GET_COM_EVENT_COUNTER };

		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x00;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadFifoQueueSuccessMalformedUnknownUnitAndUnavailable)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1)"
				    "  (fifo 10 (values 4660 22136 3))"
				    "  (fifo 20 (values)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	if (ok)
	{
		u8 req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x0A };
		u8 want[] = { FC_READ_FIFO_QUEUE, 0x00, 0x08, 0x00, 0x03, 0x12, 0x34, 0x56, 0x78, 0x00, 0x03 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == sizeof(want) &&
		     memcmp(resp, want, sizeof(want)) == 0;
	}
	if (ok)
	{
		/* non-destructive read */
		u8 req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x0A };
		u8 want[] = { FC_READ_FIFO_QUEUE, 0x00, 0x08, 0x00, 0x03, 0x12, 0x34, 0x56, 0x78, 0x00, 0x03 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == sizeof(want) &&
		     memcmp(resp, want, sizeof(want)) == 0;
	}
	if (ok)
	{
		u8 req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x14 };
		u8 want[] = { FC_READ_FIFO_QUEUE, 0x00, 0x02, 0x00, 0x00 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) && len == sizeof(want) &&
		     memcmp(resp, want, sizeof(want)) == 0;
	}
	if (ok)
	{
		u8 req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x63 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_FIFO_QUEUE, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		u8 short_req[] = { FC_READ_FIFO_QUEUE, 0x00 };
		u8 long_req[]  = { FC_READ_FIFO_QUEUE, 0x00, 0x0A, 0x00 };

		ok = exchange(&client, 1, short_req, sizeof(short_req), resp, &len) &&
		     exception_is(resp, len, FC_READ_FIFO_QUEUE, EXC_ILLEGAL_DATA_VALUE);
		if (ok)
			ok = exchange(&client, 1, long_req, sizeof(long_req), resp, &len) &&
			     exception_is(resp, len, FC_READ_FIFO_QUEUE, EXC_ILLEGAL_DATA_VALUE);
	}
	if (ok)
	{
		u8 req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x0A };

		ok = exchange(&client, 3, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_FIFO_QUEUE, EXC_GATEWAY_TARGET_FAILED);
	}
	if (ok)
	{
		/* 6 handled requests above (3 successful completions, 3 exceptions); unknown unit not counted */
		u8 req[] = { FC_GET_COM_EVENT_COUNTER };

		ok = exchange(&client, 1, req, 1, resp, &len) && len == 5 &&
		     resp[3] == 0x00 && resp[4] == 0x03;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadFifoQueueUnavailableWhenNoFifosConfigured)
{
	client_t client = { .fd = -1 };
	u8	 req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x00 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 0 (static 1)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_FIFO_QUEUE, EXC_ILLEGAL_DATA_ADDRESS);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadDeviceIdentificationMeiTypes)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1) (vendor-name \"Acme\") (product-code \"P\") "
				    "(major-minor-revision \"1.2\") (product-name \"Sim\")"
				    "        (canopen-object 5 24641 0 \"11223344\"))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     len > 7 && resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT &&
		     resp[1] == 0x0E && resp[2] == 0x01 && resp[3] == 0x82 &&
		     resp[4] == 0x00 && resp[6] == 0x03;
	}
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x04 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     len == 7 + 2 + 3 && resp[6] == 0x01 && resp[7] == 0x04 &&
		     resp[8] == 0x03 && memcmp(resp + 9, "Sim", 3) == 0;
	}
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x03 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_ENCAPSULATED_INTERFACE_TRANSPORT, EXC_ILLEGAL_DATA_ADDRESS);
	}
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x00, 0x00,
			     0x05, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     len == 14 && resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT &&
		     resp[1] == 0x0D && resp[10] == 0x00 && resp[11] == 0x02 &&
		     resp[12] == 0x11 && resp[13] == 0x22;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadDeviceIdentificationExtendedObjects)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1) (vendor-name \"Acme\")"
				    "        (device-id-object 160 \"Second\")"
				    "        (device-id-object 128 \"First\")"
				    "        (canopen-object 5 24641 0 \"11223344\"))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x03, 0x00 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     len == 41 && resp[1] == 0x0E && resp[3] == 0x83 &&
		     resp[4] == 0x00 && resp[5] == 0x00 && resp[6] == 0x05 &&
		     resp[26] == 0x80 && resp[27] == 5 && memcmp(resp + 28, "First", 5) == 0 &&
		     resp[33] == 0xA0 && resp[34] == 6 && memcmp(resp + 35, "Second", 6) == 0;
	}
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x00, 0x00,
			     0x06, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x01 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     len == 10 && resp[0] == (u8)(FC_ENCAPSULATED_INTERFACE_TRANSPORT | 0x80) &&
		     resp[1] == 0xFF && resp[3] == 0x06 && resp[4] == 0x0D &&
		     resp[5] == 0xCE && resp[6] == 0x0B && resp[9] == 0xFF;
	}
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0xA0 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     len == 15 && resp[1] == 0x0E && resp[3] == 0x83 && resp[6] == 0x01 &&
		     resp[7] == 0xA0 && resp[8] == 6 && memcmp(resp + 9, "Second", 6) == 0;
	}
	if (ok)
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x81 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_ENCAPSULATED_INTERFACE_TRANSPORT, EXC_ILLEGAL_DATA_ADDRESS);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

static bool
proc_utime_ticks(pid_t pid, unsigned long long* out)
{
	char	path[64];
	FILE*	file;
	char	buf[1024];
	char*	rparen;
	unsigned long long utime = 0;
	unsigned long long stime = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	file = fopen(path, "r");
	if (!file)
		return false;
	if (!fgets(buf, sizeof(buf), file))
	{
		fclose(file);
		return false;
	}
	fclose(file);
	rparen = strrchr(buf, ')');
	if (!rparen)
		return false;
	if (sscanf(rparen + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
		   &utime, &stime) != 2)
		return false;
	*out = utime + stime;
	return true;
}

TEST(NetPipelinedRequestsAreAnsweredPromptly)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len      = 0;
	u16	 first    = ts_next_tid();
	u16	 second   = ts_next_tid();
	u64	 start_ms = 0;
	u64	 elapsed  = 0;
	bool	 ok       = server_start("",
					"(device (unit-id 1) (holding 0 (static 11)) (holding 1 (static 22)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		int yes = 1;

		setsockopt(client.fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = send_request(&client, 1, first, req, 5);
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 1, 1);
		ok = ok && send_request(&client, 1, second, req, 5);
		start_ms = clock_now_ms();
	}
	if (ok)
		ok = recv_reply(&client, 1, first, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 11;
	if (ok)
		ok = recv_reply(&client, 1, second, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 22;
	if (ok)
		elapsed = clock_now_ms() - start_ms;
	ok = ok && elapsed < 250;

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetAcceptEmfileBackoffDoesNotBusyLoop)
{
	client_t	held[24];
	client_t	client = { .fd = -1 };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len     = 0;
	u32		opened  = 0;
	unsigned long long utime0 = 0;
	unsigned long long utime1 = 0;
	bool		ok      = false;

	for (u32 i = 0; i < (u32)(sizeof(held) / sizeof(held[0])); i++)
		held[i].fd = -1;

	/* Low RLIMIT_NOFILE: server can start, but accept() will eventually hit EMFILE.
	 * The limit must leave room for startup fds while being small enough that
	 * accepted connections exhaust the table before SERVER_MAX_CONNECTIONS.
	 * Hold only a few clients beyond that point: every extra one waits in the
	 * listen backlog and costs another 1 s accept backoff during recovery. */
	g_server_nofile = 20;
	ok = server_start("", "(device (unit-id 1) (holding 0 (static 5)))");

	for (u32 i = 0; ok && i < (u32)(sizeof(held) / sizeof(held[0])); i++)
	{
		if (!client_open(g_server.probe_port, 200, &held[i]))
		{
			held[i].fd = -1;
			break;
		}
		opened++;
	}

	if (ok)
	{
		ok = proc_utime_ticks(g_server.pid, &utime0);
		usleep(300000);
		ok = ok && proc_utime_ticks(g_server.pid, &utime1);
		/* 300ms window: a real busy loop burns far more than a few ticks.
		 * Allow generous headroom for slow CI/schedulers. */
		ok = ok && (utime1 - utime0) < 150;
	}
	ok = ok && ts_file_contains(g_server.stderr_path, "accept failed");

	/* Release pressure and verify the server recovers after backoff. */
	for (u32 i = 0; i < opened; i++)
		client_close(&held[i]);
	for (u32 i = 0; ok && i < 30; i++)
	{
		client_close(&client);
		client.fd = -1;
		if (!client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client))
		{
			usleep(100000);
			continue;
		}
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		if (exchange(&client, 1, req, 5, resp, &len) &&
		    len == 4 &&
		    register_value(resp, len, 0) == 5)
			break;
		if (i == 29)
			ok = false;
		usleep(100000);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}




TEST(NetIdleTimeoutClosesIdleClient)
{
	client_t client = { .fd = -1 };
	bool	 ok     = server_start("(idle-timeout-ms 100)",
				      "(device (unit-id 1) (holding 0 (static 7)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	ok = ok && wait_for_server_close(&client, 1500);

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetIdleTimeoutDoesNotCloseDelayedResponseClient)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len    = 0;
	u64	 start  = 0;
	u64	 elapsed = 0;
	bool	 ok     = server_start("(idle-timeout-ms 100)",
				      "(device (unit-id 1) (holding 0 (static 11))"
				      "  (fault (kind delay) (delay-ms 500) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, 2000, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		start = clock_now_ms();
		ok = exchange(&client, 1, req, 5, resp, &len);
		elapsed = clock_now_ms() - start;
	}
	ok = ok && len == 4 && register_value(resp, len, 0) == 11;
	ok = ok && elapsed >= 400;

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetIdleTimeoutFreesSlotsForNewClients)
{
	client_t clients[32];
	client_t extra = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len    = 0;
	bool	 ok     = server_start("(idle-timeout-ms 100)",
				      "(device (unit-id 1) (holding 0 (static 5)))");
	u32	 opened = 0;

	for (u32 i = 0; i < 32; i++)
	{
		clients[i].fd = -1;
		ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &clients[i]);
		if (!ok)
			break;
		opened++;
	}

	for (u32 i = 0; ok && i < opened; i++)
		ok = wait_for_server_close(&clients[i], 5000);

	/* After idle timeout closes them, a new client should be served. */
	for (u32 i = 0; ok && i < 100; i++)
	{
		client_close(&extra);
		extra.fd = -1;
		if (!client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &extra))
		{
			usleep(50000);
			continue;
		}
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		if (exchange(&extra, 1, req, 5, resp, &len) &&
		    len == 4 &&
		    register_value(resp, len, 0) == 5)
			break;
		if (i == 99)
			ok = false;
		usleep(50000);
	}

	client_close(&extra);
	for (u32 i = 0; i < opened; i++)
		client_close(&clients[i]);
	server_stop();
	ASSERT_EQ(ok && opened == 32);
}

TEST(NetTcpIpv6LoopbackServesRequest)
{
	client_t	client = { .fd = -1 };
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	bool		ok;

	if (!ts_ipv6_loopback_available())
	{
		fprintf(stderr, "SKIP: IPv6 loopback unavailable\n");
		return;
	}

	ok = server_start_with_bind("::1", "", "(device (unit-id 1) (holding 0 (static 42)))");
	ok = ok && client_open_v6(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 4 &&
		     resp[0] == FC_READ_HOLDING_REGISTERS && register_value(resp, len, 0) == 42;
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

#if WITH_TLS
TEST(NetTlsCheckRejectsInvalidTlsOptions)
{
	const char missing[] =
		"(tls (certificate \"tests/tls/missing.pem\") "
		"(private-key \"" TLS_SERVER_KEY_PATH "\") "
		"(ca-certificate \"" TLS_CA_PATH "\"))";
	const char mismatch[] =
		"(tls (certificate \"" TLS_SERVER_CERT_PATH "\") "
		"(private-key \"" TLS_CLIENT_KEY_PATH "\") "
		"(ca-certificate \"" TLS_CA_PATH "\"))";
	const char ciphers[] =
		"(tls (certificate \"" TLS_SERVER_CERT_PATH "\") "
		"(private-key \"" TLS_SERVER_KEY_PATH "\") "
		"(ca-certificate \"" TLS_CA_PATH "\") "
		"(tls1-2-ciphers \"NO_SUCH_CIPHER\"))";

	ASSERT_EQ(tls_check_run(missing, 1, "cannot load server certificate"));
	ASSERT_EQ(tls_check_run(mismatch, 1, "key values mismatch"));
	ASSERT_EQ(tls_check_run(ciphers, 1, "invalid TLS 1.2 cipher list"));
}

TEST(NetTlsCheckAcceptsValidTlsConfig)
{
	ASSERT_EQ(tls_check_run(TLS_SERVER_OPTIONS, 0, NULL));
}
#endif
