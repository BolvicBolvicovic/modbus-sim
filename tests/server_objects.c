/* Server objects: every transport is a caller-owned struct with init/step/stop/close
 * and no process-global state, so one process can host several servers and drive
 * them from its own loop. These tests run the servers in-process. */
#define _GNU_SOURCE
#include "vtest.h"
#include "build_config.h"
#include "device/device.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/ascii_server.h"
#include "net/ascii_tcp_server.h"
#include "net/rtu_server.h"
#include "net/rtu_tcp_server.h"
#include "net/tcp_server.h"
#include "tests/tests_utils/test_support.h"
#if WITH_TLS
#include "tests/tests_utils/tls_client.h"
#endif
#include "util/clock.h"
#include "vlib/arena.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum
{
	PUMP_TIMEOUT_MS = 2000,
};

static device_t*
make_device(arena_t* arena, u8 unit, u16 value)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { 0 };

	if (!dev)
		return NULL;
	dev->unit_id = unit;
	strcpy(dev->name, "objects");
	p.space = SPACE_HOLDING;
	p.address = 0;
	p.type = VT_U16;
	p.kind = POINT_STATIC;
	p.static_value = (f64)value;
	return device_add_point(dev, &p) == NULL ? dev : NULL;
}

/* Steps both TCP servers (either may be NULL) until fd is readable. */
static bool
pump_tcp(tcp_server_t* a, tcp_server_t* b, int fd)
{
	u64 deadline = clock_now_ms() + PUMP_TIMEOUT_MS;

	while (clock_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		if (a)
			(void)tcp_server_step(a, 1);
		if (b)
			(void)tcp_server_step(b, 1);
		if (poll(&pfd, 1, 0) > 0)
			return true;
	}
	return false;
}

static bool
read_holding_tcp(tcp_server_t* a, tcp_server_t* b, int fd, u16 tid, u16* value)
{
	u8	req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8	resp[MODBUS_MAX_PDU];
	u32	len = 0;

	if (!ts_mbap_send(fd, 1, tid, req, sizeof(req)) ||
	    !pump_tcp(a, b, fd) ||
	    !ts_mbap_recv(fd, 1, tid, resp, &len, 1000) ||
	    len != 4 ||
	    resp[0] != FC_READ_HOLDING_REGISTERS)
		return false;
	*value = (u16)((resp[2] << 8) | resp[3]);
	return true;
}

static tcp_server_options_t
tcp_options(device_t** devs, u32 count, u16 port)
{
	return (tcp_server_options_t){
		.bind_address = "127.0.0.1",
		.port = port,
		.seed = 1,
		.devices = devs,
		.device_count = count,
	};
}

TEST(ServerObjectsTwoTcpServersInOneProcess)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev_a = make_device(arena, 1, 0x1111);
	device_t*		dev_b = make_device(arena, 1, 0x2222);
	tcp_server_t		sa;
	tcp_server_t		sb;
	tcp_server_options_t	oa = tcp_options(&dev_a, 1, ts_free_port());
	tcp_server_options_t	ob = tcp_options(&dev_b, 1, ts_free_port());
	u16			va = 0;
	u16			vb = 0;
	int			ca;
	int			cb;

	ASSERT_EQ(dev_a != NULL && dev_b != NULL);
	ASSERT_EQ(oa.port != 0 && ob.port != 0 && oa.port != ob.port);
	ASSERT_EQ(tcp_server_init(&sa, &oa) == 0);
	ASSERT_EQ(tcp_server_init(&sb, &ob) == 0);
	ca = ts_connect_loopback(oa.port, AF_INET, 1000);
	cb = ts_connect_loopback(ob.port, AF_INET, 1000);
	ASSERT_EQ(ca >= 0 && cb >= 0);

	/* Each server answers from its own device, stepped alternately by the test. */
	for (u16 tid = 1; tid <= 3; tid++)
	{
		ASSERT_EQ(read_holding_tcp(&sa, &sb, ca, tid, &va));
		ASSERT_EQ(read_holding_tcp(&sa, &sb, cb, tid, &vb));
		ASSERT_EQ(va == 0x1111);
		ASSERT_EQ(vb == 0x2222);
	}

	/* Stopping one leaves the other serving. */
	tcp_server_stop(&sa);
	ASSERT_EQ(tcp_server_step(&sa, 0) == NET_STEP_DONE);
	ASSERT_EQ(read_holding_tcp(NULL, &sb, cb, 9, &vb));
	ASSERT_EQ(vb == 0x2222);

	close(ca);
	close(cb);
	tcp_server_close(&sa);
	tcp_server_close(&sb);
	arena_release(arena);
}

TEST(ServerObjectsStepZeroIsNonblockingAndWaitIsBounded)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena, 1, 1);
	tcp_server_t		s;
	tcp_server_options_t	o = tcp_options(&dev, 1, ts_free_port());
	u64			t0;
	u64			elapsed;

	ASSERT_EQ(dev != NULL && o.port != 0);
	ASSERT_EQ(tcp_server_init(&s, &o) == 0);

	t0 = clock_now_ms();
	for (u32 i = 0; i < 20; i++)
		ASSERT_EQ(tcp_server_step(&s, 0) == NET_STEP_CONTINUE);
	elapsed = clock_now_ms() - t0;
	ASSERT_EQ(elapsed < 100);	/* idle servers would otherwise wait 1 s per step */

	t0 = clock_now_ms();
	ASSERT_EQ(tcp_server_step(&s, 60) == NET_STEP_CONTINUE);
	elapsed = clock_now_ms() - t0;
	ASSERT_EQ(elapsed >= 40 && elapsed < 500);

	tcp_server_close(&s);
	arena_release(arena);
}

TEST(ServerObjectsStopEndsSteppingAndCloseIsIdempotent)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena, 1, 1);
	tcp_server_t		s;
	tcp_server_t		again;
	tcp_server_options_t	o = tcp_options(&dev, 1, ts_free_port());
	u64			t0;

	ASSERT_EQ(dev != NULL && o.port != 0);
	ASSERT_EQ(tcp_server_init(&s, &o) == 0);
	ASSERT_EQ(tcp_server_step(&s, 0) == NET_STEP_CONTINUE);
	tcp_server_stop(&s);
	t0 = clock_now_ms();
	ASSERT_EQ(tcp_server_step(&s, -1) == NET_STEP_DONE);
	ASSERT_EQ(clock_now_ms() - t0 < 200);	/* does not wait for the poll timeout */
	tcp_server_close(&s);
	tcp_server_close(&s);

	/* The listener is really gone: the same port can be bound again. */
	ASSERT_EQ(tcp_server_init(&again, &o) == 0);
	tcp_server_close(&again);
	arena_release(arena);
}

TEST(ServerObjectsInitFailureReportsAndLeavesNothingOpen)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena, 1, 1);
	tcp_server_t		first;
	tcp_server_t		second;
	tcp_server_options_t	o = tcp_options(&dev, 1, ts_free_port());

	ASSERT_EQ(tcp_server_init(&first, &o) == 0);
	ASSERT_EQ(tcp_server_init(&second, &o) != 0);	/* port in use */
	tcp_server_close(&second);
	tcp_server_close(&first);
	arena_release(arena);
}

static tcp_server_t* volatile g_child_server;

static void
child_on_term(int signo)
{
	(void)signo;
	tcp_server_stop(g_child_server);
}

TEST(ServerObjectsRunReturnsPromptlyWhenHostStopsFromSignalHandler)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena, 1, 1);
	tcp_server_options_t	o = tcp_options(&dev, 1, ts_free_port());
	pid_t			pid;
	int			probe;
	int			status = 0;
	u64			t0;

	ASSERT_EQ(dev != NULL && o.port != 0);
	pid = fork();
	ASSERT_EQ(pid >= 0);
	if (pid == 0)
	{
		static tcp_server_t	server;
		struct sigaction	sa = { 0 };

		/* Same arrangement as the binary: the host owns the handler. */
		g_child_server = &server;
		sa.sa_handler = child_on_term;
		sigaction(SIGTERM, &sa, NULL);
		_exit(tcp_server_run(&server, &o) == 0 ? 0 : 3);
	}

	probe = -1;
	for (u32 i = 0; i < 200 && probe < 0; i++)	/* wait for the child's listener */
	{
		probe = ts_connect_loopback(o.port, AF_INET, 200);
		if (probe < 0)
			usleep(10000);
	}
	if (probe < 0)
		kill(pid, SIGKILL);
	ASSERT_EQ(probe >= 0);
	usleep(50000);	/* let the child accept and enter poll() */
	t0 = clock_now_ms();
	kill(pid, SIGTERM);
	ASSERT_EQ(ts_wait_pid(pid, &status, 1000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	ASSERT_EQ(clock_now_ms() - t0 < 500);
	close(probe);
	arena_release(arena);
}

static bool
rtu_exchange(rtu_server_t** servers, u32 n, int fd, u8 unit, u16* value)
{
	u8	req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8	resp[MODBUS_MAX_PDU];
	u8	addr = 0;
	u32	len = 0;
	u64	deadline = clock_now_ms() + PUMP_TIMEOUT_MS;

	if (!ts_rtu_send(fd, unit, req, sizeof(req), true))
		return false;
	while (clock_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		for (u32 i = 0; i < n; i++)
			(void)rtu_server_step(servers[i], 1);
		if (poll(&pfd, 1, 0) > 0)
			break;
	}
	if (!ts_rtu_recv(fd, &addr, resp, &len, 500, true) ||
	    addr != unit ||
	    len != 4)
		return false;
	*value = (u16)((resp[2] << 8) | resp[3]);
	return true;
}

TEST(ServerObjectsTwoRtuServersSteppedFromHostEndOnEofOrStop)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev_a = make_device(arena, 1, 0xAAAA);
	device_t*		dev_b = make_device(arena, 1, 0xBBBB);
	int			pa[2];
	int			pb[2];
	rtu_server_t		sa;
	rtu_server_t		sb;
	rtu_server_t*		both[2] = { &sa, &sb };
	rtu_server_options_t	oa = { .seed = 1, .frame_gap_ms = 2, .devices = &dev_a, .device_count = 1 };
	rtu_server_options_t	ob = { .seed = 1, .frame_gap_ms = 2, .devices = &dev_b, .device_count = 1 };
	u16			va = 0;
	u16			vb = 0;

	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pa) == 0);
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pb) == 0);
	oa.fd = pa[1];
	ob.fd = pb[1];
	ASSERT_EQ(rtu_server_init(&sa, &oa) == 0);
	ASSERT_EQ(rtu_server_init(&sb, &ob) == 0);

	ASSERT_EQ(rtu_exchange(both, 2, pa[0], 1, &va));
	ASSERT_EQ(rtu_exchange(both, 2, pb[0], 1, &vb));
	ASSERT_EQ(va == 0xAAAA && vb == 0xBBBB);

	/* Peer EOF ends only that server; stop ends the other. */
	close(pa[0]);
	{
		s32 rc = NET_STEP_CONTINUE;

		for (u32 i = 0; i < 50 && rc == NET_STEP_CONTINUE; i++)
			rc = rtu_server_step(&sa, 10);
		ASSERT_EQ(rc == NET_STEP_DONE);
	}
	ASSERT_EQ(rtu_server_step(&sb, 0) == NET_STEP_CONTINUE);
	rtu_server_stop(&sb);
	ASSERT_EQ(rtu_server_step(&sb, 0) == NET_STEP_DONE);
	rtu_server_close(&sa);
	rtu_server_close(&sa);
	rtu_server_close(&sb);
	close(pa[1]);
	close(pb[0]);
	close(pb[1]);
	arena_release(arena);
}

TEST(ServerObjectsAsciiServerSteppedFromHost)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena, 1, 0x0A0B);
	int			fds[2];
	ascii_server_t		s;
	ascii_server_options_t	o = { .seed = 1, .devices = &dev, .device_count = 1 };
	u8			req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];
	u8			addr = 0;
	u32			len = 0;
	u64			deadline = clock_now_ms() + PUMP_TIMEOUT_MS;

	ASSERT_EQ(dev != NULL);
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	o.fd = fds[1];
	ASSERT_EQ(ascii_server_init(&s, &o) == 0);
	ASSERT_EQ(ts_ascii_send(fds[0], 1, req, sizeof(req), true));
	while (clock_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fds[0], .events = POLLIN };

		ASSERT_EQ(ascii_server_step(&s, 1) == NET_STEP_CONTINUE);
		if (poll(&pfd, 1, 0) > 0)
			break;
	}
	ASSERT_EQ(ts_ascii_recv_response(fds[0], &addr, resp, &len, 500, true));
	ASSERT_EQ(addr == 1 && len == 4 && resp[2] == 0x0A && resp[3] == 0x0B);

	ascii_server_stop(&s);
	ASSERT_EQ(ascii_server_step(&s, -1) == NET_STEP_DONE);
	ascii_server_close(&s);
	ascii_server_close(&s);
	close(fds[0]);
	close(fds[1]);
	arena_release(arena);
}

TEST(ServerObjectsRtuTcpAndAsciiTcpStopAndReopen)
{
	arena_t*			arena = ARENA_ALLOC();
	device_t*			dev   = make_device(arena, 1, 1);
	rtu_tcp_server_t		r;
	ascii_tcp_server_t		a;
	rtu_tcp_server_options_t	ro = { .bind_address = "127.0.0.1", .port = ts_free_port(),
					       .seed = 1, .devices = &dev, .device_count = 1 };
	ascii_tcp_server_options_t	ao = { .bind_address = "127.0.0.1", .port = ts_free_port(),
					       .seed = 1, .devices = &dev, .device_count = 1 };
	u64				t0;

	ASSERT_EQ(rtu_tcp_server_init(&r, &ro) == 0);
	ASSERT_EQ(ascii_tcp_server_init(&a, &ao) == 0);
	t0 = clock_now_ms();
	for (u32 i = 0; i < 10; i++)
	{
		ASSERT_EQ(rtu_tcp_server_step(&r, 0) == NET_STEP_CONTINUE);
		ASSERT_EQ(ascii_tcp_server_step(&a, 0) == NET_STEP_CONTINUE);
	}
	ASSERT_EQ(clock_now_ms() - t0 < 100);
	rtu_tcp_server_stop(&r);
	ascii_tcp_server_stop(&a);
	ASSERT_EQ(rtu_tcp_server_step(&r, -1) == NET_STEP_DONE);
	ASSERT_EQ(ascii_tcp_server_step(&a, -1) == NET_STEP_DONE);
	rtu_tcp_server_close(&r);
	rtu_tcp_server_close(&r);
	ascii_tcp_server_close(&a);
	ascii_tcp_server_close(&a);

	ASSERT_EQ(rtu_tcp_server_init(&r, &ro) == 0);
	ASSERT_EQ(ascii_tcp_server_init(&a, &ao) == 0);
	rtu_tcp_server_close(&r);
	ascii_tcp_server_close(&a);
	arena_release(arena);
}

#if WITH_TLS
#define TLS_CA		"tests/tls/ca.pem"
#define TLS_CERT	"tests/tls/server.pem"
#define TLS_KEY		"tests/tls/server.key"
#define TLS_CLIENT_CERT	"tests/tls/client.pem"
#define TLS_CLIENT_KEY	"tests/tls/client.key"

/* Runs in a forked child: the blocking TLS client needs the parent's loop to
 * keep stepping the servers while it handshakes. */
static void
tls_child(u16 port, u16 expected)
{
	tls_client_t	client = { .fd = -1, .ssl = NULL };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MBAP_MAX_FRAME];
	u8		raw[MBAP_HEADER_SIZE + 4];
	u32		n = ts_mbap_encode(frame, 1, 7, req, sizeof(req));
	bool		ok;

	ok = tls_client_open(port, TLS_CLIENT_CERT, TLS_CLIENT_KEY, TLS_CA, &client) &&
	     tls_client_write_all(&client, frame, MBAP_HEADER_SIZE + (n - MBAP_HEADER_SIZE)) &&
	     tls_client_read_exact(&client, raw, sizeof(raw)) &&
	     raw[MBAP_HEADER_SIZE] == FC_READ_HOLDING_REGISTERS &&
	     (u16)((raw[MBAP_HEADER_SIZE + 2] << 8) | raw[MBAP_HEADER_SIZE + 3]) == expected;
	tls_client_close(&client);
	_exit(ok ? 0 : 1);
}

TEST(ServerObjectsTwoTlsServersInOneProcessKeepSeparateSessionPools)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev_a = make_device(arena, 1, 0x1234);
	device_t*		dev_b = make_device(arena, 1, 0x5678);
	static tcp_server_t	sa;
	static tcp_server_t	sb;
	tcp_server_options_t	oa = tcp_options(&dev_a, 1, ts_free_port());
	tcp_server_options_t	ob = tcp_options(&dev_b, 1, ts_free_port());
	pid_t			pids[2];
	bool			done[2] = { false, false };
	bool			ok[2]   = { false, false };
	u64			deadline = clock_now_ms() + 5000;

	oa.tls_enabled = ob.tls_enabled = true;
	oa.tls = (net_tls_config_t){ .certificate = TLS_CERT, .private_key = TLS_KEY,
				     .ca_certificate = TLS_CA,
				     .require_client_certificate = true };
	ob.tls = oa.tls;
	ASSERT_EQ(dev_a != NULL && dev_b != NULL);
	ASSERT_EQ(tcp_server_init(&sa, &oa) == 0);
	ASSERT_EQ(tcp_server_init(&sb, &ob) == 0);

	pids[0] = fork();
	ASSERT_EQ(pids[0] >= 0);
	if (pids[0] == 0)
		tls_child(oa.port, 0x1234);
	pids[1] = fork();
	ASSERT_EQ(pids[1] >= 0);
	if (pids[1] == 0)
		tls_child(ob.port, 0x5678);

	while ((!done[0] || !done[1]) && clock_now_ms() < deadline)
	{
		(void)tcp_server_step(&sa, 1);
		(void)tcp_server_step(&sb, 1);
		for (u32 i = 0; i < 2; i++)
		{
			int status = 0;

			if (!done[i] && waitpid(pids[i], &status, WNOHANG) == pids[i])
			{
				done[i] = true;
				ok[i] = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			}
		}
	}
	for (u32 i = 0; i < 2; i++)
	{
		if (!done[i])
		{
			kill(pids[i], SIGKILL);
			waitpid(pids[i], NULL, 0);
		}
	}
	tcp_server_close(&sa);
	tcp_server_close(&sb);
	arena_release(arena);
	ASSERT_EQ(ok[0] && ok[1]);
}
#endif
