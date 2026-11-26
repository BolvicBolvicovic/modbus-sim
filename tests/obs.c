/* Observability: request metrics, Prometheus rendering and the HTTP endpoint, driven in-process. */
#define _GNU_SOURCE
#include "vtest.h"
#include "net/net_obs.h"
#include "modbus/pdu.h"
#include "tests/tests_utils/test_support.h"
#include "util/clock.h"
#include "vlib/arena.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static device_t*
make_device(arena_t* arena, u8 unit, const char* name)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { .kind = POINT_STATIC, .space = SPACE_HOLDING, .type = VT_U16 };

	dev->unit_id = unit;
	snprintf(dev->name, sizeof(dev->name), "%s", name);
	device_add_point(dev, &p);
	return dev;
}

static const u8 OK_RESP[]  = { FC_READ_HOLDING_REGISTERS, 0x02, 0x00, 0x2A };
static const u8 EXC_RESP[] = { FC_READ_HOLDING_REGISTERS | 0x80, EXC_ILLEGAL_DATA_ADDRESS };

TEST(ObsCountsRequestsByOutcomeDeviceAndFault)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		devs[] = { make_device(arena, 1, "a"), make_device(arena, 2, "b") };
	net_obs_t		obs;
	fault_decision_t	delayed = { .delay_ms = 30 };
	fault_decision_t	dropped = { .drop = true };

	ASSERT_EQ(net_obs_init(&obs, "tcp", devs, 2, NULL, 0) == 0);
	net_obs_request(&obs, "peer", devs[0], 1, 3, OK_RESP, sizeof(OK_RESP), &delayed);
	net_obs_request(&obs, "peer", devs[1], 2, 3, EXC_RESP, sizeof(EXC_RESP), NULL);
	net_obs_request(&obs, "peer", devs[1], 2, 6, OK_RESP, sizeof(OK_RESP), &dropped);
	net_obs_request(&obs, NULL, NULL, 0, 6, NULL, 0, NULL);		/* broadcast */
	net_obs_rejected(&obs);
	net_obs_connection(&obs, true);
	net_obs_connection(&obs, true);
	net_obs_connection(&obs, false);

	ASSERT_EQ(obs.metrics.requests_by_fc[3] == 2 && obs.metrics.requests_by_fc[6] == 2);
	ASSERT_EQ(obs.metrics.responses[NET_RESULT_OK] == 1 &&
		  obs.metrics.responses[NET_RESULT_EXCEPTION] == 1 &&
		  obs.metrics.responses[NET_RESULT_DROPPED] == 1 &&
		  obs.metrics.responses[NET_RESULT_NONE] == 1);
	ASSERT_EQ(obs.metrics.device[0].requests == 1 && obs.metrics.device[1].requests == 2);
	ASSERT_EQ(obs.metrics.device[1].exceptions[EXC_ILLEGAL_DATA_ADDRESS] == 1);
	ASSERT_EQ(obs.metrics.device[0].faults[FAULT_DELAY] == 1 && obs.metrics.device[1].faults[FAULT_DROP] == 1);
	/* Only answered requests have a latency: 30 ms lands in the 50 ms bucket, 0 ms in the first. */
	ASSERT_EQ(obs.metrics.latency_count == 2 && obs.metrics.latency_sum_ms == 30);
	ASSERT_EQ(obs.metrics.latency_bucket[0] == 1 && obs.metrics.latency_bucket[4] == 1);
	ASSERT_EQ(obs.metrics.frames_rejected == 1);
	ASSERT_EQ(obs.metrics.connections_total == 2 && obs.metrics.connections_active == 1);
	net_obs_close(&obs);
	arena_release(arena);
}

TEST(ObsRendersPrometheusText)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	devs[] = { make_device(arena, 7, "Meter \"A\"\\1") };
	net_obs_t	obs;
	char		out[NET_OBS_HTTP_OUT];
	char		tiny[64];
	u32		len;

	ASSERT_EQ(net_obs_init(&obs, "rtu-udp", devs, 1, NULL, 0) == 0);
	obs.ready = true;
	net_obs_request(&obs, NULL, devs[0], 7, 3, EXC_RESP, sizeof(EXC_RESP), NULL);
	len = net_obs_render_metrics(&obs, out, sizeof(out));
	ASSERT_EQ(len > 0 && len == strlen(out));
	ASSERT_EQ(strstr(out, "mogbus_info{version=\"") != NULL && strstr(out, "transport=\"rtu-udp\"} 1\n") != NULL);
	ASSERT_EQ(strstr(out, "\nmogbus_ready 1\n") != NULL);
	ASSERT_EQ(strstr(out, "\nmogbus_requests_total{fc=\"3\"} 1\n") != NULL);
	ASSERT_EQ(strstr(out, "\nmogbus_responses_total{result=\"exception\"} 1\n") != NULL);
	ASSERT_EQ(strstr(out, "\nmogbus_device_exceptions_total{unit=\"7\",code=\"2\"} 1\n") != NULL);
	ASSERT_EQ(strstr(out, "mogbus_device_info{unit=\"7\",name=\"Meter \\\"A\\\"\\\\1\"} 1\n") != NULL);
	ASSERT_EQ(strstr(out, "mogbus_response_latency_seconds_bucket{le=\"+Inf\"} 1\n") != NULL);
	ASSERT_EQ(strstr(out, "# TYPE mogbus_response_latency_seconds histogram\n") != NULL);
	ASSERT_EQ(net_obs_render_metrics(&obs, tiny, sizeof(tiny)) == 0);
	net_obs_close(&obs);
	arena_release(arena);
}

/* True when the peer closed the connection: EOF, or a reset because the server closed with
 * unread input (an oversized request). */
static bool
closed_by_peer(int fd, u32 timeout_ms)
{
	struct pollfd	pfd = { .fd = fd, .events = POLLIN };
	char		c;

	return poll(&pfd, 1, (int)timeout_ms) > 0 && recv(fd, &c, 1, 0) <= 0;
}

/* Sends one HTTP request and steps net_poll() until the response is complete. */
static bool
http_get(net_obs_t* obs, u16 port, const char* request, char* out, u32 cap)
{
	int	fd = ts_connect_loopback(port, AF_INET, 1000);
	u32	len = 0;
	u64	deadline = clock_now_ms() + 2000;

	if (fd < 0)
		return false;
	if (send(fd, request, strlen(request), MSG_NOSIGNAL) != (ssize_t)strlen(request))
	{
		close(fd);
		return false;
	}
	while (clock_now_ms() < deadline && len + 1 < cap)
	{
		struct pollfd	pfd = { .fd = fd, .events = POLLIN };
		ssize_t		got;

		(void)net_poll(obs, NULL, 0, 5);
		if (poll(&pfd, 1, 0) <= 0)
			continue;
		got = recv(fd, out + len, cap - 1 - len, 0);
		if (got <= 0)
			break;
		len += (u32)got;
	}
	out[len] = '\0';
	close(fd);
	return len > 0;
}

TEST(ObsHttpServesHealthReadinessAndMetrics)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	devs[] = { make_device(arena, 1, "a") };
	net_obs_t	obs;
	u16		port = ts_free_port();
	static char	resp[NET_OBS_HTTP_OUT + 512];

	ASSERT_EQ(port != 0 && net_obs_init(&obs, "tcp", devs, 1, "127.0.0.1", port) == 0);
	ASSERT_EQ(http_get(&obs, port, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 && strstr(resp, "\r\n\r\nok\n") != NULL);
	ASSERT_EQ(http_get(&obs, port, "GET /readyz HTTP/1.1\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strncmp(resp, "HTTP/1.1 503 ", 13) == 0);	/* not ready until the transport runs */
	obs.ready = true;
	ASSERT_EQ(http_get(&obs, port, "GET /readyz?verbose HTTP/1.0\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
	ASSERT_EQ(http_get(&obs, port, "GET /metrics HTTP/1.1\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strstr(resp, "Content-Type: text/plain; version=0.0.4") != NULL &&
		  strstr(resp, "\nmogbus_ready 1\n") != NULL);
	ASSERT_EQ(http_get(&obs, port, "HEAD /metrics HTTP/1.1\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strstr(resp, "Content-Length: ") != NULL && strstr(resp, "mogbus_ready") == NULL);
	ASSERT_EQ(http_get(&obs, port, "GET /other HTTP/1.1\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strncmp(resp, "HTTP/1.1 404 ", 13) == 0);
	ASSERT_EQ(http_get(&obs, port, "POST /metrics HTTP/1.1\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strncmp(resp, "HTTP/1.1 405 ", 13) == 0);
	net_obs_close(&obs);
	arena_release(arena);
}

TEST(ObsHttpClosesStalledAndOversizedClients)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	devs[] = { make_device(arena, 1, "a") };
	net_obs_t	obs;
	u16		port = ts_free_port();
	char		big[NET_OBS_HTTP_IN + 16];
	char		resp[256];
	int		stalled[NET_OBS_HTTP_CONNS];
	int		fd;
	u64		deadline;

	ASSERT_EQ(port != 0 && net_obs_init(&obs, "tcp", devs, 1, "127.0.0.1", port) == 0);
	/* Fill every slot with a client that never sends; they must not block a later probe for
	 * longer than the connection timeout. */
	for (u32 i = 0; i < NET_OBS_HTTP_CONNS; i++)
	{
		stalled[i] = ts_connect_loopback(port, AF_INET, 1000);
		ASSERT_EQ(stalled[i] >= 0);
	}
	deadline = clock_now_ms() + NET_OBS_HTTP_TIMEOUT_MS + 500;
	while (clock_now_ms() < deadline)
		(void)net_poll(&obs, NULL, 0, 20);
	for (u32 i = 0; i < NET_OBS_HTTP_CONNS; i++)
	{
		ASSERT_EQ(ts_wait_eof(stalled[i], 500));
		close(stalled[i]);
	}
	ASSERT_EQ(http_get(&obs, port, "GET /healthz HTTP/1.1\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);

	/* A request head larger than the buffer is closed without a response. */
	memset(big, 'a', sizeof(big));
	memcpy(big, "GET /", 5);
	fd = ts_connect_loopback(port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0 && send(fd, big, sizeof(big), MSG_NOSIGNAL) == (ssize_t)sizeof(big));
	deadline = clock_now_ms() + 1000;
	while (clock_now_ms() < deadline)
		(void)net_poll(&obs, NULL, 0, 5);
	ASSERT_EQ(closed_by_peer(fd, 500));
	close(fd);
	net_obs_close(&obs);
	arena_release(arena);
}

TEST(ObsInitFailsWhenThePortIsTaken)
{
	net_obs_t	a;
	net_obs_t	b;
	u16		port = ts_free_port();

	ASSERT_EQ(net_obs_init(&a, "tcp", NULL, 0, "127.0.0.1", port) == 0);
	ASSERT_EQ(net_obs_init(&b, "tcp", NULL, 0, "127.0.0.1", port) == -1);
	net_obs_close(&b);
	net_obs_close(&a);
}
