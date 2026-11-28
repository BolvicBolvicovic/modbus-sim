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
#include "os/os_time.h"
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
/* Extra CLI flags for the next server_start (cleared by server_stop). */
static bool g_server_json_logs = false;
static bool g_server_web = false;

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
	char*	    argv[] = { (char*)(bin ? bin : "./mogbus"), "--check=verbose",
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

/* One FC 03 read of `address` on `unit`; true when the reply is exactly that one register. */
static bool
reads_register(client_t* client, u8 unit, u16 address, u16 want)
{
	u8  req[5];
	u8  resp[MODBUS_MAX_PDU];
	u32 len = 0;

	read_pdu(req, FC_READ_HOLDING_REGISTERS, address, 1);
	return exchange(client, unit, req, 5, resp, &len) &&
	       len == 4 &&
	       resp[0] == FC_READ_HOLDING_REGISTERS &&
	       register_value(resp, len, 0) == want;
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
		char*       json[] = { (char*)(bin ? bin : "./mogbus"), "-v", "--log-format", "json",
				       g_server.config, NULL };
		char*       web[] = { (char*)(bin ? bin : "./mogbus"), "-w", g_server.config, NULL };

		return ts_server_start(&g_server, g_server_json_logs ? json : g_server_web ? web : argv);
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
	g_server_json_logs = false;
	g_server_web = false;
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
	char	    out[512];

	return ts_exec(argv, NULL, out, sizeof(out));
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
		start = os_now_ms();
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
		elapsed = os_now_ms() - start;
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

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client) &&
	     reads_register(&client, 1, 0, 0x1111) &&
	     reads_register(&client, 2, 0, 0x2222);
	if (ok)
	{
		read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
		ok = exchange(&client, 3, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_READ_HOLDING_REGISTERS, EXC_GATEWAY_TARGET_FAILED);
	}

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
	ok = ok && reads_register(&client, 1, 0, 11) && reads_register(&client, 2, 0, 22);

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetAnyUnitDeviceAnswersUnknownUnitsAndExactMatchWins)
{
	client_t client = { .fd = -1 };
	bool	 ok   = server_start("",
				     "(device (unit-id 1) (holding 0 (static 11)))\n"
				     "(device (unit-id 5) (any-unit on) (holding 0 (static 55)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	/* exchange() only succeeds when the reply echoes the request's unit id */
	ok = ok && reads_register(&client, 1, 0, 11) && reads_register(&client, 5, 0, 55) &&
	     reads_register(&client, 9, 0, 55) && reads_register(&client, 0, 0, 55) &&
	     reads_register(&client, 255, 0, 55);

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
	ok = ok && reads_register(&client, 1, 0, 0x1234);

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
		ok = reads_register(&probe, 1, 0, 9);
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
		ok = reads_register(&probe, 1, 0, 9);
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
		start = os_now_ms();
		ok = send_request(&delayed, 1, delayed_tid, req, 5);
		ok = ok && send_request(&fast, 2, fast_tid, req, 5);
	}
	if (ok)
	{
		ok = recv_reply(&fast, 2, fast_tid, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 22;
		fast_ms = os_now_ms() - start;
	}
	if (ok)
	{
		ok = recv_reply(&delayed, 1, delayed_tid, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 11;
		delayed_ms = os_now_ms() - start;
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

/* One server, one device per fault combination: each unit checks that its faults reach the wire
 * (the fault decisions themselves are covered in device.c/pdu.c). */
TEST(NetFaultsReachTheWire)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	u64	 start;
	bool	 ok  = server_start("(seed 3)",
				    "(device (unit-id 1) (holding 0 (static 42))"
				    "  (fault (kind exception) (code 6) (percent 100)))"
				    "(device (unit-id 2) (holding 0 (static 42))"
				    "  (fault (kind corrupt) (percent 100)))"
				    "(device (unit-id 3) (holding 0 (static 42))"
				    "  (fault (kind delay) (delay-ms 300) (percent 100))"
				    "  (fault (kind corrupt) (percent 100)))"
				    "(device (unit-id 4) (holding 0 (static 42))"
				    "  (fault (kind exception) (code 6) (percent 100))"
				    "  (fault (kind corrupt) (percent 100)))"
				    "(device (unit-id 5) (holding 0 (static 42))"
				    "  (fault (kind drop) (percent 100))"
				    "  (fault (kind exception) (code 6) (percent 100)))");

	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);

	/* exception alone */
	ok = ok && exchange(&client, 1, req, 5, resp, &len) &&
	     exception_is(resp, len, FC_READ_HOLDING_REGISTERS, EXC_SLAVE_DEVICE_BUSY);
	/* corrupt alone: the byte count is inverted, the data is intact */
	ok = ok && exchange(&client, 2, req, 5, resp, &len) &&
	     len == 4 &&
	     resp[0] == FC_READ_HOLDING_REGISTERS &&
	     resp[1] == (0x02 ^ 0xFF) &&
	     register_value(resp, len, 0) == 42;
	/* delay then corrupt */
	start = os_now_ms();
	ok = ok && exchange(&client, 3, req, 5, resp, &len) &&
	     os_now_ms() - start >= 300 &&
	     len == 4 &&
	     resp[0] == FC_READ_HOLDING_REGISTERS &&
	     resp[1] == (0x02 ^ 0xFF) &&
	     register_value(resp, len, 0) == 42;
	/* exception, then corrupt */
	ok = ok && exchange(&client, 4, req, 5, resp, &len) &&
	     len == 2 &&
	     resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80) &&
	     resp[1] == (EXC_SLAVE_DEVICE_BUSY ^ 0xFF);
	/* drop wins over exception: no reply */
	ok = ok && send_request(&client, 5, ts_next_tid(), req, 5) &&
	     ts_no_data(client.fd, 200);

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
		ok = reads_register(&clients[0], 1, 0, 5);
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

#ifndef _WIN32	/* /proc and RLIMIT_NOFILE */
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
		start_ms = os_now_ms();
	}
	if (ok)
		ok = recv_reply(&client, 1, first, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 11;
	if (ok)
		ok = recv_reply(&client, 1, second, resp, &len) &&
		     len == 4 && register_value(resp, len, 0) == 22;
	if (ok)
		elapsed = os_now_ms() - start_ms;
	ok = ok && elapsed < 250;

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetAcceptEmfileBackoffDoesNotBusyLoop)
{
	client_t	held[24];
	client_t	client = { .fd = -1 };
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
		if (reads_register(&client, 1, 0, 5))
			break;
		if (i == 29)
			ok = false;
		usleep(100000);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}
#endif

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
		start = os_now_ms();
		ok = exchange(&client, 1, req, 5, resp, &len);
		elapsed = os_now_ms() - start;
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
		if (reads_register(&extra, 1, 0, 5))
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
	bool		ok;

	if (!ts_ipv6_loopback_available())
	{
		fprintf(stderr, "SKIP: IPv6 loopback unavailable\n");
		return;
	}

	ok = server_start_with_bind("::1", "", "(device (unit-id 1) (holding 0 (static 42)))");
	ok = ok && client_open_v6(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	ok = ok && reads_register(&client, 1, 0, 42);

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

/* One raw HTTP/1.1 GET against the running binary's endpoint; the server closes after it. */
static bool
http_raw(u16 port, const char* req, char* out, u32 cap)
{
	int	fd = ts_connect_loopback(port, AF_INET, 1000);
	u32	len = 0;

	if (fd < 0)
		return false;
	ts_set_recv_timeout(fd, 2000);
	if (!ts_send_socket(fd, (const u8*)req, (u32)strlen(req)))
	{
		close(fd);
		return false;
	}
	while (len + 1 < cap)
	{
		ssize_t got = recv(fd, out + len, cap - 1 - len, 0);

		if (got <= 0)
			break;
		len += (u32)got;
	}
	out[len] = '\0';
	close(fd);
	return len > 0;
}

static bool
http_get_binary(u16 port, const char* path, char* out, u32 cap)
{
	char req[128];

	snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", path);
	return http_raw(port, req, out, cap);
}

TEST(NetHttpEndpointServesMetricsAndJsonLogs)
{
	client_t	client = { .fd = -1 };
	u16		http_port = ts_free_port();
	char		options[64];
	static char	text[40000];
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	bool		ok;

	snprintf(options, sizeof(options), "(http (port %u))", http_port);
	g_server_json_logs = true;
	ok = server_start(options, "(device (unit-id 1) (holding 0 (static 5))"
				   " (fault (kind delay) (delay-ms 20) (percent 100)))");
	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);
	read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
	ok = ok && exchange(&client, 1, req, 5, resp, &len) && len == 4;
	read_pdu(req, FC_READ_HOLDING_REGISTERS, 9, 1);
	ok = ok && exchange(&client, 1, req, 5, resp, &len) && resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80);
	client_close(&client);
	ok = ok && http_get_binary(http_port, "/readyz", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	ok = ok && http_get_binary(http_port, "/metrics", text, sizeof(text));
	ok = ok && strstr(text, "\nmogbus_requests_total{fc=\"3\"} 2\n") != NULL;
	ok = ok && strstr(text, "\nmogbus_device_exceptions_total{unit=\"1\",code=\"2\"} 1\n") != NULL;
	ok = ok && strstr(text, "\nmogbus_device_faults_total{unit=\"1\",kind=\"delay\"} 2\n") != NULL;
	ok = ok && strstr(text, "\nmogbus_response_latency_seconds_bucket{le=\"0.01\"} 0\n") != NULL;
	ok = ok && strstr(text, "\nmogbus_response_latency_seconds_bucket{le=\"0.025\"} 2\n") != NULL;
	ok = ok && ts_file_contains(g_server.stderr_path, "\"event\":\"request\",\"transport\":\"tcp\"");
	ok = ok && ts_file_contains(g_server.stderr_path,
				    "\"result\":\"exception\",\"exception\":2,\"latency_ms\":20,\"faults\":[\"delay\"]");
	ok = ok && ts_file_contains(g_server.stderr_path, "{\"ts\":\"");
	server_stop();
	ASSERT_EQ(ok);
}

/* One HTTP request with a JSON body against the running binary's endpoint. */
static bool
http_send_binary(u16 port, const char* method, const char* path, const char* body, char* out, u32 cap)
{
	char	req[512];
	int	fd = ts_connect_loopback(port, AF_INET, 1000);
	u32	len = 0;

	if (fd < 0)
		return false;
	snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %zu\r\n\r\n%s", method,
		 path, strlen(body), body);
	ts_set_recv_timeout(fd, 2000);
	if (!ts_send_socket(fd, (const u8*)req, (u32)strlen(req)))
	{
		close(fd);
		return false;
	}
	while (len + 1 < cap)
	{
		ssize_t got = recv(fd, out + len, cap - 1 - len, 0);

		if (got <= 0)
			break;
		len += (u32)got;
	}
	out[len] = '\0';
	close(fd);
	return len > 0;
}

TEST(NetHttpControlChangesRegistersAndFaults)
{
	client_t	client = { .fd = -1 };
	u16		http_port = ts_free_port();
	char		options[96];
	static char	text[4096];
	u8		req[8];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	bool		ok;

	snprintf(options, sizeof(options), "(http (port %u) (control on))", http_port);
	ok = server_start(options, "(device (unit-id 1) (holding 0 (static 5)))");
	ok = ok && ts_file_contains(g_server.stderr_path, "HTTP control API enabled");
	ok = ok && client_open(g_server.probe_port, NET_RECV_TIMEOUT_MS, &client);

	/* A holding register written over HTTP is what a Modbus client reads next. */
	ok = ok && http_send_binary(http_port, "PUT", "/api/devices/1/holding/0", "{\"values\":[77]}",
				    text, sizeof(text)) && strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	read_pdu(req, FC_READ_HOLDING_REGISTERS, 0, 1);
	ok = ok && exchange(&client, 1, req, 5, resp, &len) && len == 4 && resp[3] == 77;

	/* An exception fault set over HTTP shows up on the wire and is gone after DELETE. */
	ok = ok && http_send_binary(http_port, "PUT", "/api/devices/1/faults/exception",
				    "{\"exception_code\":6}", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	ok = ok && exchange(&client, 1, req, 5, resp, &len) && len == 2 &&
	     resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80) && resp[1] == 6;
	ok = ok && http_send_binary(http_port, "DELETE", "/api/devices/1/faults/exception", "",
				    text, sizeof(text)) && strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	ok = ok && exchange(&client, 1, req, 5, resp, &len) && len == 4 && resp[3] == 77;

	/* The Modbus side is visible over HTTP, and the endpoint counts its requests. */
	ok = ok && http_send_binary(http_port, "GET", "/api/devices/1/holding/0", "", text, sizeof(text)) &&
	     strstr(text, "{\"values\":[77]}") != NULL;
	ok = ok && http_send_binary(http_port, "GET", "/api/devices/1/holding/4", "", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 422 ", 13) == 0;
	ok = ok && http_get_binary(http_port, "/metrics", text, sizeof(text)) &&
	     strstr(text, "\nmogbus_control_requests_total{result=\"ok\"} 4\n") != NULL &&
	     strstr(text, "\nmogbus_control_requests_total{result=\"error\"} 1\n") != NULL;
	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

/* `mogbus -w` turns the UI and the control API on for a config that only has (http ...). */
TEST(NetWebFlagServesPageAndGuardsApi)
{
	u16		http_port = ts_free_port();
	char		options[64];
	char		req[256];
	static char	text[262144];	/* the page is about 72 KB */
	const char*	body;
	unsigned	length = 0;
	bool		ok;

	snprintf(options, sizeof(options), "(http (port %u))", http_port);
	g_server_web = true;
	ok = server_start(options, "(device (unit-id 1) (holding 0 (static 5)))");
	ok = ok && ts_file_contains(g_server.stderr_path, "HTTP control API enabled");
	snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", http_port);
	ok = ok && http_raw(http_port, req, text, sizeof(text)) && strncmp(text, "HTTP/1.1 200 OK", 15) == 0 &&
	     strstr(text, "Content-Type: text/html; charset=utf-8\r\n") != NULL &&
	     strstr(text, "Content-Security-Policy: ") != NULL;
	body = ok ? strstr(text, "\r\n\r\n") : NULL;
	ok = ok && body && strstr(text, "Content-Length: ") &&
	     sscanf(strstr(text, "Content-Length: ") + 16, "%u", &length) == 1 &&
	     length > 100 && strlen(body + 4) == length &&
	     strncmp(body + 4, "<!doctype html>", 15) == 0 && strstr(body, "</html>") != NULL;
	snprintf(req, sizeof(req), "GET /api/devices HTTP/1.1\r\nHost: localhost:%u\r\n\r\n", http_port);
	ok = ok && http_raw(http_port, req, text, sizeof(text)) && strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	/* A rebinding name is refused, on the page and on the API. */
	ok = ok && http_raw(http_port, "GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 403 ", 13) == 0;
	ok = ok && http_raw(http_port, "GET /api/devices HTTP/1.1\r\nHost: evil.example\r\n\r\n", text,
			    sizeof(text)) && strncmp(text, "HTTP/1.1 403 ", 13) == 0;
	ok = ok && http_get_binary(http_port, "/healthz", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	server_stop();
	ASSERT_EQ(ok);
}

/* (http (ui on) (allowed-host ...)) in the config, without any CLI flag. */
TEST(NetHttpUiFromConfigAndAllowedHost)
{
	u16		http_port = ts_free_port();
	char		options[128];
	static char	text[8192];
	bool		ok;

	snprintf(options, sizeof(options), "(http (port %u) (ui on) (allowed-host \"sim.lab\"))", http_port);
	ok = server_start(options, "(device (unit-id 1) (holding 0 (static 5)))");
	ok = ok && ts_file_contains(g_server.stderr_path, "HTTP control API enabled");
	ok = ok && http_raw(http_port, "GET / HTTP/1.1\r\nHost: SIM.lab:9102\r\n\r\n", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	ok = ok && http_raw(http_port, "GET /api/devices HTTP/1.1\r\nHost: sim.lab\r\n\r\n", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 200 OK", 15) == 0;
	ok = ok && http_raw(http_port, "GET / HTTP/1.1\r\nHost: other.lab\r\n\r\n", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 403 ", 13) == 0;
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetHttpPageIsNotServedWithoutUi)
{
	u16		http_port = ts_free_port();
	char		options[64];
	static char	text[4096];
	bool		ok;

	snprintf(options, sizeof(options), "(http (port %u) (control on))", http_port);
	ok = server_start(options, "(device (unit-id 1) (holding 0 (static 5)))");
	ok = ok && http_raw(http_port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", text, sizeof(text)) &&
	     strncmp(text, "HTTP/1.1 404 ", 13) == 0;
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetHttpControlIsOffUnlessEnabled)
{
	u16		http_port = ts_free_port();
	char		options[64];
	static char	text[4096];
	bool		ok;

	snprintf(options, sizeof(options), "(http (port %u))", http_port);
	ok = server_start(options, "(device (unit-id 1) (holding 0 (static 5)))");
	ok = ok && http_send_binary(http_port, "PUT", "/api/devices/1/holding/0", "{\"values\":[77]}",
				    text, sizeof(text)) && strncmp(text, "HTTP/1.1 404 ", 13) == 0;
	ok = ok && !ts_file_contains(g_server.stderr_path, "control API enabled");
	server_stop();
	ASSERT_EQ(ok);
}

/* The HTTP endpoint opens before the transport: a taken port fails start-up with status 2. */
TEST(NetHttpPortInUseFailsToStart)
{
	int	busy = -1;
	u16	http_port = 0;
	char	options[64];
	ts_server_t second = { .pid = -1, .master_fd = -1, .label = "http clash",
			       .flags = TS_CAPTURE_STDERR | TS_NULL_STDIN | TS_EXIT_IS_READY };
	char	config[256];
	const char* bin = getenv("MOGBUS_BIN");
	char*	argv[3];
	int	status = 0;
	bool	ok;

	ASSERT_EQ(server_start("", "(device (unit-id 1) (holding 0 (static 5)))"));
	busy = ts_connect_loopback(g_server.probe_port, AF_INET, 1000);
	ASSERT_EQ(busy >= 0);
	close(busy);
	http_port = g_server.probe_port;	/* the running Modbus listener holds this TCP port */
	snprintf(options, sizeof(options), "(http (port %u))", http_port);
	snprintf(config, sizeof(config), "(server (mode tcp) (port %u) %s)\n(device (unit-id 1) (holding 0))\n",
		 ts_free_port(), options);
	ASSERT_EQ(ts_write_config(&second, "/tmp/mogbus-http-XXXXXX", config));
	argv[0] = (char*)(bin ? bin : "./mogbus");
	argv[1] = second.config;
	argv[2] = NULL;
	ASSERT_EQ(ts_server_start(&second, argv));
	ok = ts_wait_pid(second.pid, &status, 5000);
	second.pid = -1;
	ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 2;
	ok = ok && ts_file_contains(second.stderr_path, "cannot listen for HTTP");
	ts_server_cleanup(&second);
	ASSERT_EQ(ok);
	server_stop();
}
