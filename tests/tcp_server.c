/* Network integration tests: run the real server binary and talk to it over loopback.
 * Each test starts a server on a private port and stops it before asserting, so a failed
 * assertion cannot leave a child behind. PRE_TEST/atexit clean up leftovers from failures. */
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "util/clock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define NET_RECV_TIMEOUT_MS	1000
#define NET_SHORT_TIMEOUT_MS	100
#define NET_START_ATTEMPTS	300
#define NET_START_TIMEOUT_MS	5000
#define NET_MAX_ATTEMPTS	24

typedef struct client_t client_t;
struct client_t
{
	int	fd;
};

typedef struct fixture_t fixture_t;
struct fixture_t
{
	pid_t	pid;
	u16	port;
	char	config[80];
};

static fixture_t	g_server = { .pid = -1 };
static u32		g_server_nofile = 0;
static char		g_server_stderr[80];

static bool
server_verbose(void)
{
	const char* v = getenv("MODBUS_SIM_TEST_VERBOSE");

	return v && v[0] && strcmp(v, "0") != 0;
}

static void
server_dump_stderr(void)
{
	FILE*	file;
	char	line[512];

	if (!g_server_stderr[0])
		return;

	file = fopen(g_server_stderr, "r");
	if (!file)
		return;

	fprintf(stderr, "--- server stderr (%s) ---\n", g_server_stderr);
	while (fgets(line, sizeof(line), file))
		fputs(line, stderr);
	fprintf(stderr, "--- end server stderr ---\n");
	fclose(file);
}

static void
server_stop(void);

static void
server_cleanup(void);

/* --- socket helpers --- */

static bool
client_open(u16 port, int timeout_ms, client_t* out)
{
	struct sockaddr_in	addr = { 0 };
	struct timeval		tv   = { .tv_sec = timeout_ms / 1000,
					 .tv_usec = (timeout_ms % 1000) * 1000 };
	int			fd   = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return false;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		close(fd);
		return false;
	}

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
	while (len > 0)
	{
		ssize_t sent = send(fd, buf, len, MSG_NOSIGNAL);

		if (sent <= 0)
			return false;
		buf += sent;
		len -= (u32)sent;
	}
	return true;
}

static bool
recv_exact(int fd, u8* buf, u32 len)
{
	while (len > 0)
	{
		ssize_t got = recv(fd, buf, len, 0);

		if (got <= 0)
			return false;
		buf += got;
		len -= (u32)got;
	}
	return true;
}

static u16
next_tid(void)
{
	static u16 tid = 0;

	return ++tid;
}

static bool
send_request(client_t* client, u8 unit, u16 tid, const u8* pdu, u32 pdu_len)
{
	u8		frame[MBAP_MAX_FRAME];
	mbap_header_t	header = { .transaction = tid, .length = (u16)(pdu_len + 1), .unit = unit };

	mbap_encode(frame, &header);
	memcpy(frame + MBAP_HEADER_SIZE, pdu, pdu_len);
	return send_all(client->fd, frame, MBAP_HEADER_SIZE + pdu_len);
}

static bool
recv_reply(client_t* client, u8 unit, u16 tid, u8* pdu, u32* pdu_len)
{
	u8		raw[MBAP_HEADER_SIZE];
	mbap_header_t	header = { 0 };

	if (!recv_exact(client->fd, raw, sizeof(raw)))
		return false;

	mbap_decode(raw, &header);
	if (!mbap_is_valid(&header) ||
	    header.transaction != tid ||
	    header.unit != unit)
		return false;

	*pdu_len = header.length - 1;
	return recv_exact(client->fd, pdu, *pdu_len);
}

static bool
exchange(client_t* client, u8 unit, const u8* req, u32 req_len, u8* resp, u32* resp_len)
{
	u16 tid = next_tid();

	return send_request(client, unit, tid, req, req_len) &&
	       recv_reply(client, unit, tid, resp, resp_len);
}

static u32
read_pdu(u8* pdu, u8 function, u16 address, u16 quantity)
{
	pdu[0] = function;
	put_be16(pdu + 1, address);
	put_be16(pdu + 3, quantity);
	return 5;
}

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
	u64	start = clock_now_ms();

	while (clock_now_ms() - start < timeout_ms)
	{
		char	byte = 0;
		ssize_t	got  = recv(client->fd, &byte, 1, MSG_DONTWAIT);

		if (got == 0)
			return true;
		if (got < 0 &&
		    errno != EAGAIN &&
		    errno != EWOULDBLOCK &&
		    errno != EINTR)
			return true;
		usleep(10000);
	}
	return false;
}

/* --- server fixture --- */

/* Asks the kernel for an unused loopback port. There is a small race before the child
 * binds it, but it keeps tests independent of a hard-coded port number. */
static u16
find_free_port(void)
{
	struct sockaddr_in	addr = { 0 };
	socklen_t		len  = sizeof(addr);
	int			fd   = socket(AF_INET, SOCK_STREAM, 0);
	u16			port = 0;

	if (fd < 0)
		return 0;

	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
	    getsockname(fd, (struct sockaddr*)&addr, &len) == 0)
		port = ntohs(addr.sin_port);

	close(fd);
	return port;
}

static void
server_stop(void)
{
	fixture_t* fx     = &g_server;
	pid_t	   pid    = fx->pid;
	int	   status = 0;
	bool	   reaped = false;
	bool	   ok     = true;

	fx->pid = -1;
	if (pid > 0)
	{
		kill(pid, SIGTERM);
		for (u32 i = 0; i < 200; i++)
		{
			pid_t done = waitpid(pid, &status, WNOHANG);

			if (done == pid)
			{
				reaped = true;
				break;
			}
			if (done < 0 && errno == ECHILD)
			{
				ok = false;
				fprintf(stderr, "net test: server pid %d already reaped\n", (int)pid);
				break;
			}
			usleep(10000);
		}
		if (!reaped &&
		    ok &&
		    waitpid(pid, &status, WNOHANG) != pid)
		{
			kill(pid, SIGKILL);
			reaped = waitpid(pid, &status, 0) == pid;
		}
		if (reaped)
			ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}

	if (!ok)
	{
		server_dump_stderr();
		fprintf(stderr, "net test: server pid %d exited=%d status=%d signal=%d\n",
			(int)pid, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
			WIFSIGNALED(status) ? WTERMSIG(status) : -1);
	}

	if (fx->config[0])
	{
		unlink(fx->config);
		fx->config[0] = '\0';
	}
	if (g_server_stderr[0])
	{
		unlink(g_server_stderr);
		g_server_stderr[0] = '\0';
	}
	g_server_nofile = 0;
	ASSERT_EQ(ok);
}

static void
server_cleanup(void)
{
	/* atexit cleanup: best-effort stop without asserting test context. */
	fixture_t* fx = &g_server;
	pid_t	   pid = fx->pid;

	fx->pid = -1;
	if (pid > 0)
	{
		kill(pid, SIGTERM);
		for (u32 i = 0; i < 200; i++)
		{
			int   st   = 0;
			pid_t done = waitpid(pid, &st, WNOHANG);

			if (done == pid || (done < 0 && errno == ECHILD))
				goto done;
			usleep(10000);
		}
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}
done:
	if (fx->config[0])
	{
		unlink(fx->config);
		fx->config[0] = '\0';
	}
	if (g_server_stderr[0])
	{
		unlink(g_server_stderr);
		g_server_stderr[0] = '\0';
	}
	g_server_nofile = 0;
}

static bool
file_contains(const char* path, const char* needle)
{
	FILE*	file = fopen(path, "r");
	char	line[512];

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file))
	{
		if (strstr(line, needle))
		{
			fclose(file);
			return true;
		}
	}
	fclose(file);
	return false;
}

/* `server_options` is appended inside (server ...); pass "" for defaults. */
static bool
server_start(const char* server_options, const char* devices)
{
	fixture_t*	fx       = &g_server;
	char		config[2048];
	const char*	bin      = NULL;
	FILE*		file     = NULL;
	int		fd       = -1;

	fx->port = find_free_port();
	if (!fx->port)
	{
		fprintf(stderr, "net test: no free port\n");
		return false;
	}

	snprintf(config, sizeof(config), "(server (port %u) (bind \"127.0.0.1\") %s)\n%s\n",
		 fx->port, server_options ? server_options : "", devices);

	strcpy(fx->config, "/tmp/modbus-sim-net-XXXXXX");
	fd = mkstemp(fx->config);
	if (fd < 0)
	{
		fprintf(stderr, "net test: mkstemp failed: %s\n", strerror(errno));
		return false;
	}

	file = fdopen(fd, "w");
	if (!file)
	{
		close(fd);
		unlink(fx->config);
		fx->config[0] = '\0';
		return false;
	}
	fputs(config, file);
	fclose(file);

	if (!server_verbose())
	{
		strcpy(g_server_stderr, "/tmp/modbus-sim-neterr-XXXXXX");
		fd = mkstemp(g_server_stderr);
		if (fd < 0)
		{
			fprintf(stderr, "net test: mkstemp stderr failed: %s\n", strerror(errno));
			unlink(fx->config);
			fx->config[0] = '\0';
			g_server_stderr[0] = '\0';
			g_server_nofile = 0;
			return false;
		}
		close(fd);
	}

	bin = getenv("MODBUS_SIM_BIN");
	if (!bin)
		bin = "./modbus-sim";

	fx->pid = fork();
	if (fx->pid == 0)
	{
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (g_server_stderr[0] && !server_verbose())
		{
			if (!freopen(g_server_stderr, "w", stderr))
				_exit(127);
			setvbuf(stderr, NULL, _IONBF, 0);
		}
		if (g_server_nofile > 0)
		{
			struct rlimit lim = { .rlim_cur = g_server_nofile, .rlim_max = g_server_nofile };

			if (setrlimit(RLIMIT_NOFILE, &lim) < 0)
				_exit(126);
		}
		execl(bin, bin, fx->config, (char*)NULL);
		_exit(127);
	}
	if (fx->pid < 0)
	{
		fprintf(stderr, "net test: fork failed: %s\n", strerror(errno));
		unlink(fx->config);
		fx->config[0] = '\0';
		return false;
	}

	if (g_server_stderr[0] && !server_verbose())
	{
		/* The server logs this just after listen(), before entering its poll loop.
		 * Avoid a probe connection: it would occupy a client slot that capacity
		 * tests need, and reaping it would require another blind sleep. */
		u64 deadline = clock_now_ms() + NET_START_TIMEOUT_MS;

		while (clock_now_ms() < deadline)
		{
			if (file_contains(g_server_stderr, "listening on"))
				return true;
			usleep(2000);
		}
	}
	else
	{
		/* Verbose runs send stderr to the console; fall back to connect probing. */
		for (u32 i = 0; i < NET_START_ATTEMPTS; i++)
		{
			client_t probe = { .fd = -1 };

			if (client_open(fx->port, 50, &probe))
			{
				client_close(&probe);
				usleep(50000);	/* let the server reap the probe connection */
				return true;
			}
			usleep(10000);
		}
	}

	{
		int status = 0;

		fprintf(stderr, "net test: server %u never accepted on %s\n", fx->port, fx->config);
		pid_t done = waitpid(fx->pid, &status, WNOHANG);
		fprintf(stderr, "net test: child waitpid=%d exited=%d status=%d\n", (int)done,
			WIFEXITED(status), WEXITSTATUS(status));
		server_dump_stderr();
	}
	server_stop();
	return false;
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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
	u16	 first  = next_tid();
	u16	 second = next_tid();
	bool	 ok     = server_start("",
				      "(device (unit-id 1) (holding 0 (static 11)) (holding 1 (static 22)))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

TEST(NetUnitIdRoutingAndGatewayException)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1) (holding 0 (static 4369)))\n"
				    "(device (unit-id 2) (holding 0 (static 8738)))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

TEST(NetExceptionsOverWire)
{
	client_t client = { .fd = -1 };
	u8	 req[8];
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("", "(device (unit-id 1) (holding 1) (input 0))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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
		/* Diagnostics subfunction 0x0000 echoes the request; others are unsupported. */
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, 0x0000);
		put_be16(req + 3, 0xBEEF);
		ok = exchange(&client, 1, req, 5, resp, &len) && len == 5 &&
		     memcmp(resp, req, 5) == 0;
	}
	if (ok)
	{
		put_be16(req + 1, 0x0001);
		ok = exchange(&client, 1, req, 5, resp, &len) &&
		     exception_is(resp, len, FC_DIAGNOSTICS, EXC_ILLEGAL_FUNCTION);
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

		ok = client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

		ok = client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
	u16		tid1 = next_tid();
	u16		tid2 = next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (static 4369)) (holding 1 (static 8738)))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

	/* FC 01: 2000 bits -> response PDU is 252 bytes (max read bits). */
	if (ok)
	{
		mbap_header_t	h = { .transaction = next_tid(), .length = 6, .unit = 1 };

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
		mbap_header_t	h = { .transaction = next_tid(), .length = 6, .unit = 1 };

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
		mbap_header_t	h = { .transaction = next_tid(), .length = 253, .unit = 1 };

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
		mbap_header_t	h = { .transaction = next_tid(), .length = 6, .unit = 1 };

		mbap_encode(req, &h);
		read_pdu(req + MBAP_HEADER_SIZE, FC_READ_HOLDING_REGISTERS, 122, 1);
		ok = send_all(client.fd, req, MBAP_HEADER_SIZE + 5) &&
		     recv_reply(&client, 1, h.transaction, resp, &len);
		ok = ok && len == 4 && register_value(resp, len, 0) == 0x2000 + 122;
	}

	/* FC 15: 1968 bits -> request PDU is 252 bytes (max write bits). */
	if (ok)
	{
		mbap_header_t	h = { .transaction = next_tid(), .length = 253, .unit = 1 };

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
		mbap_header_t	h = { .transaction = next_tid(), .length = 6, .unit = 1 };

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
	u16		tid = next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (count 125) (static 13124)))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
	u16		tid = next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (static 9))"
					 "  (fault (kind delay) (delay-ms 300) (percent 100)))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
		ok = client_open(g_server.port, NET_RECV_TIMEOUT_MS, &probe);
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
	u16		tid = next_tid();
	bool		ok = server_start("",
					 "(device (unit-id 1) (holding 0 (static 9))"
					 "  (fault (kind delay) (delay-ms 300) (percent 100)))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
		ok = client_open(g_server.port, NET_RECV_TIMEOUT_MS, &probe);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &delayed);
	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &fast);

	if (ok)
	{
		delayed_tid = next_tid();
		fast_tid    = next_tid();
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

	ok = ok && client_open(g_server.port, NET_SHORT_TIMEOUT_MS, &client);

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
		ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &clients[i]);
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
		bool	 open  = client_open(g_server.port, NET_SHORT_TIMEOUT_MS, &extra);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
		ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);

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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
	if (ok)
	{
		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_READ_FIFO_QUEUE, EXC_ILLEGAL_DATA_ADDRESS);
	}

	client_close(&client);
	server_stop();
	ASSERT_EQ(ok);
}

TEST(NetReadDeviceIdentificationMei14)
{
	client_t client = { .fd = -1 };
	u8	 resp[MODBUS_MAX_PDU];
	u32	 len = 0;
	bool	 ok  = server_start("",
				    "(device (unit-id 1) (vendor-name \"Acme\") (product-code \"P\") "
				    "(major-minor-revision \"1.2\") (product-name \"Sim\"))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x01, 0x00 };

		ok = exchange(&client, 1, req, sizeof(req), resp, &len) &&
		     exception_is(resp, len, FC_ENCAPSULATED_INTERFACE_TRANSPORT, EXC_ILLEGAL_FUNCTION);
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
	u16	 first    = next_tid();
	u16	 second   = next_tid();
	u64	 start_ms = 0;
	u64	 elapsed  = 0;
	bool	 ok       = server_start("",
					"(device (unit-id 1) (holding 0 (static 11)) (holding 1 (static 22)))");

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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
	client_t	held[64];
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
	 * accepted connections exhaust the table before SERVER_MAX_CONNECTIONS. */
	g_server_nofile = 20;
	ok = server_start("", "(device (unit-id 1) (holding 0 (static 5)))");

	for (u32 i = 0; ok && i < (u32)(sizeof(held) / sizeof(held[0])); i++)
	{
		if (!client_open(g_server.port, 200, &held[i]))
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
	ok = ok && file_contains(g_server_stderr, "accept failed");

	/* Release pressure and verify the server recovers after backoff. */
	for (u32 i = 0; i < opened; i++)
		client_close(&held[i]);
	for (u32 i = 0; ok && i < 30; i++)
	{
		client_close(&client);
		client.fd = -1;
		if (!client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client))
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

	ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &client);
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

	ok = ok && client_open(g_server.port, 2000, &client);
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
		ok = ok && client_open(g_server.port, NET_RECV_TIMEOUT_MS, &clients[i]);
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
		if (!client_open(g_server.port, NET_RECV_TIMEOUT_MS, &extra))
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
