/* Observability: request metrics, Prometheus rendering and the HTTP endpoint, driven in-process. */
#define _GNU_SOURCE
#include "vtest.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "modbus/pdu.h"
#include "os/os_time.h"
#include "vlib/arena.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include "tests/tests_utils/test_support.h"
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32	/* the HTTP and control tests below drive real loopback sockets with POSIX calls */
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
	u64	deadline = os_now_ms() + 2000;

	if (fd < 0)
		return false;
	if (send(fd, request, strlen(request), MSG_NOSIGNAL) != (ssize_t)strlen(request))
	{
		close(fd);
		return false;
	}
	while (os_now_ms() < deadline && len + 1 < cap)
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
	deadline = os_now_ms() + NET_OBS_HTTP_TIMEOUT_MS + 500;
	while (os_now_ms() < deadline)
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
	deadline = os_now_ms() + 1000;
	while (os_now_ms() < deadline)
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

/* ---- Runtime control API ---- */

static device_t*
make_control_device(arena_t* arena)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { .kind = POINT_STATIC, .space = SPACE_HOLDING, .type = VT_U16 };

	dev->unit_id = 3;
	snprintf(dev->name, sizeof(dev->name), "ctl \"dev\"");
	device_add_point(dev, &p);			/* holding 0 */
	p.address      = 10;
	p.type         = VT_F32;
	p.static_value = 1.5;
	device_add_point(dev, &p);			/* holding 10-11 */
	p.space        = SPACE_COIL;
	p.type         = VT_BOOL;
	p.address      = 0;
	p.static_value = 0;
	device_add_point(dev, &p);
	return dev;
}

/* One control request with a JSON body; the response (head and body) lands in `out`. */
static bool
control(net_obs_t* obs, u16 port, const char* method, const char* path, const char* body,
	char* out, u32 cap)
{
	char request[NET_OBS_HTTP_IN + NET_OBS_HTTP_BODY + 128];

	snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", method,
		 path, strlen(body), body);
	return http_get(obs, port, request, out, cap);
}

static bool
status_is(const char* resp, int code)
{
	char prefix[16];

	snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", code);
	return strncmp(resp, prefix, strlen(prefix)) == 0;
}

static const char*
body_of(const char* resp)
{
	const char* p = strstr(resp, "\r\n\r\n");

	return p ? p + 4 : "";
}

TEST(ObsControlIsOffByDefault)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	devs[] = { make_control_device(arena) };
	net_obs_t	obs;
	u16		port = ts_free_port();
	u16		value = 1;
	char		resp[1024];

	ASSERT_EQ(port != 0 && net_obs_init(&obs, "tcp", devs, 1, "127.0.0.1", port) == 0);
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", "{\"values\":[9]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(control(&obs, port, "DELETE", "/api/anything", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(device_read(devs[0], SPACE_HOLDING, 0, 1, &value) && value == 0);
	net_obs_close(&obs);
	arena_release(arena);
}

TEST(ObsControlWritesReadOnlyPoints)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	devs[] = { make_control_device(arena) };
	net_obs_t	obs;
	u16		port = ts_free_port();
	u16		value = 0;
	static char	resp[NET_OBS_HTTP_OUT];

	devs[0]->points[0].read_only = true;
	ASSERT_EQ(port != 0 && net_obs_init(&obs, "tcp", devs, 1, "127.0.0.1", port) == 0);
	obs.control.enabled = true;

	/* The host plays the physical side: a raw PUT writes a point clients cannot. */
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", "{\"values\":[9]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	ASSERT_EQ(device_read(devs[0], SPACE_HOLDING, 0, 1, &value) && value == 9);
	ASSERT_EQ(!device_client_write(devs[0], SPACE_HOLDING, 0, 1, &value));
	net_obs_close(&obs);
	arena_release(arena);
}

TEST(ObsControlReadsAndWritesDevices)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	devs[] = { make_control_device(arena) };
	net_obs_t	obs;
	u16		port = ts_free_port();
	static char	resp[NET_OBS_HTTP_OUT];

	ASSERT_EQ(port != 0 && net_obs_init(&obs, "tcp", devs, 1, "127.0.0.1", port) == 0);
	obs.control.enabled = true;

	ASSERT_EQ(control(&obs, port, "GET", "/api/devices", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200) && strstr(resp, "Content-Type: application/json") != NULL);
	ASSERT_EQ(strcmp(body_of(resp), "[{\"unit\":3,\"name\":\"ctl \\\"dev\\\"\","
				       "\"points\":{\"coil\":1,\"discrete\":0,\"input\":0,\"holding\":2}}]") == 0);

	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", " { \"values\" : [ 7 ] } ", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200) && strcmp(body_of(resp), "{\"ok\":true}") == 0);
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/holding/0", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200) && strcmp(body_of(resp), "{\"values\":[7]}") == 0);
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/holding/10?count=2", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200) && strcmp(body_of(resp), "{\"values\":[16320,0]}") == 0);

	/* Bits take 0 and 1 only. */
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/coil/0", "{\"values\":[2]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/coil/0", "{\"values\":[1]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/coil/0", "", resp, sizeof(resp)));
	ASSERT_EQ(strcmp(body_of(resp), "{\"values\":[1]}") == 0);

	/* Decoded values: get, host-side set; the point is then host-owned and a raw write is refused. */
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/holding/10/value", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200) && strcmp(body_of(resp), "{\"value\":1.5}") == 0);
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/10/value", "{\"value\": -12.25}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/holding/10/value", "", resp, sizeof(resp)));
	ASSERT_EQ(strcmp(body_of(resp), "{\"value\":-12.25}") == 0);
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/10", "{\"values\":[1,2]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0/value", "{\"values\":[1]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));

	/* Errors: unknown unit/space/path, wrong method, undefined address, bad numbers and JSON. */
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/9/holding/0", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/flash/0", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(control(&obs, port, "GET", "/api/nothing", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(control(&obs, port, "POST", "/api/devices", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 405));
	ASSERT_EQ(control(&obs, port, "DELETE", "/api/devices/3/holding/0", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 405));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/holding/5", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422) && strstr(resp, "\"error\"") != NULL);
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/holding/70000", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/holding/0?count=126", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/coil/0?count=2001", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", "{\"values\":[1,", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", "{\"values\":[65536]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", "{\"values\":[0x10]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", "{\"values\":[1],\"values\":[2]}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(obs.metrics.control_requests[0] > 0 && obs.metrics.control_requests[1] > 0);
	net_obs_close(&obs);
	arena_release(arena);
}

TEST(ObsControlSetsAndClearsFaults)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		devs[] = { make_control_device(arena) };
	net_obs_t		obs;
	u16			port = ts_free_port();
	static char		resp[NET_OBS_HTTP_OUT];
	rng_t			rng;
	static const u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			out[260];
	fault_decision_t	decision;

	ASSERT_EQ(port != 0 && net_obs_init(&obs, "tcp", devs, 1, "127.0.0.1", port) == 0);
	obs.control.enabled  = true;
	obs.control.tcp_like = true;
	rng_seed(&rng, 1);

	net_handle_pdu(&obs, NULL, devs[0], devs[0]->unit_id, &rng, 1, req, sizeof(req), out, &decision);
	ASSERT_EQ(decision.exception_code == 0);
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/exception", "{\"exception_code\":6}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	net_handle_pdu(&obs, NULL, devs[0], devs[0]->unit_id, &rng, 2, req, sizeof(req), out, &decision);
	ASSERT_EQ(decision.exception_code == 6);
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/faults", "", resp, sizeof(resp)));
	ASSERT_EQ(strcmp(body_of(resp), "{\"faults\":[{\"kind\":\"exception\",\"percent\":100,"
					"\"scheduled\":false,\"exception_code\":6}]}") == 0);

	/* A later PUT replaces the kind; percent 0 never fires. */
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/exception", "{\"exception_code\":4,\"percent\":0}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	net_handle_pdu(&obs, NULL, devs[0], devs[0]->unit_id, &rng, 3, req, sizeof(req), out, &decision);
	ASSERT_EQ(decision.exception_code == 0);

	ASSERT_EQ(control(&obs, port, "DELETE", "/api/devices/3/faults/exception", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200) && (devs[0]->faults.present & FAULT_BIT(FAULT_EXCEPTION)) == 0);
	ASSERT_EQ(control(&obs, port, "DELETE", "/api/devices/3/faults/exception", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/faults", "", resp, sizeof(resp)));
	ASSERT_EQ(strcmp(body_of(resp), "{\"faults\":[]}") == 0);

	/* Every kind takes its own fields and validates like the config. */
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/delay", "{\"delay_ms\":250}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	net_handle_pdu(&obs, NULL, devs[0], devs[0]->unit_id, &rng, 4, req, sizeof(req), out, &decision);
	ASSERT_EQ(decision.delay_ms == 250);
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/drop", "{}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	net_handle_pdu(&obs, NULL, devs[0], devs[0]->unit_id, &rng, 5, req, sizeof(req), out, &decision);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/corrupt", "{\"mode\":\"register\"}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200) && devs[0]->faults.by_kind[FAULT_CORRUPT].corrupt_mode == FAULT_CORRUPT_REGISTER);
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/disconnect", "{}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/restart", "{\"percent\":0}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/sensor-disconnect", "{}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/delay", "{}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/delay", "{\"delay_ms\":0}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/delay", "{\"delay_ms\":60001}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/exception", "{\"exception_code\":9}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/exception", "{\"exception_code\":12}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/drop", "{\"percent\":101}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/drop", "{\"delay_ms\":5}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/corrupt", "{\"mode\":\"nope\"}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/mystery", "{}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 404));
	ASSERT_EQ(control(&obs, port, "GET", "/api/devices/3/faults/drop", "", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 405));

	/* Transport rules: no checksum in MBAP, no disconnect without connections. */
	obs.control.tcp_like = false;
	obs.control.mbap     = true;
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/disconnect", "{}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/faults/corrupt", "{\"mode\":\"checksum\"}", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 422));
	net_obs_close(&obs);
	arena_release(arena);
}

TEST(ObsControlLimitsAndCountsRequests)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	devs[] = { make_control_device(arena) };
	net_obs_t	obs;
	u16		port = ts_free_port();
	u16		value = 0;
	static char	resp[NET_OBS_HTTP_OUT];
	static char	full[NET_OBS_HTTP_BODY + 1];
	static const char head[] = "PUT /api/devices/3/holding/0 HTTP/1.1\r\ncontent-length: 14\r\n\r\n";
	int		fd;
	u32		len = 0;
	u64		deadline;

	ASSERT_EQ(port != 0 && net_obs_init(&obs, "tcp", devs, 1, "127.0.0.1", port) == 0);
	obs.control.enabled = true;

	ASSERT_EQ(http_get(&obs, port, "PUT /api/devices/3/holding/0 HTTP/1.1\r\nContent-Length: 1025\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 413));
	ASSERT_EQ(http_get(&obs, port, "PUT /api/devices/3/holding/0 HTTP/1.1\r\nContent-Length: abc\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 400));

	/* A body of exactly the limit is accepted. */
	memset(full, ' ', NET_OBS_HTTP_BODY);
	memcpy(full, "{\"values\":[4]}", 14);
	full[NET_OBS_HTTP_BODY] = '\0';
	ASSERT_EQ(control(&obs, port, "PUT", "/api/devices/3/holding/0", full, resp, sizeof(resp)));
	ASSERT_EQ(status_is(resp, 200));
	ASSERT_EQ(device_read(devs[0], SPACE_HOLDING, 0, 1, &value) && value == 4);

	/* A body that arrives after the head is waited for. */
	fd = ts_connect_loopback(port, AF_INET, 1000);
	ASSERT_EQ(fd >= 0 && send(fd, head, sizeof(head) - 1, MSG_NOSIGNAL) == (ssize_t)sizeof(head) - 1);
	for (u32 i = 0; i < 5; i++)
		(void)net_poll(&obs, NULL, 0, 10);
	ASSERT_EQ(send(fd, "{\"values\":[5]}", 14, MSG_NOSIGNAL) == 14);
	deadline = os_now_ms() + 2000;
	while (os_now_ms() < deadline && len == 0)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		(void)net_poll(&obs, NULL, 0, 5);
		if (poll(&pfd, 1, 0) > 0)
			len = (u32)recv(fd, resp, sizeof(resp) - 1, 0);
	}
	resp[len] = '\0';
	close(fd);
	ASSERT_EQ(len > 0 && status_is(resp, 200));
	ASSERT_EQ(device_read(devs[0], SPACE_HOLDING, 0, 1, &value) && value == 5);

	/* Two ok (the full body and the split one), two errors (413 and 400). */
	ASSERT_EQ(http_get(&obs, port, "GET /metrics HTTP/1.1\r\n\r\n", resp, sizeof(resp)));
	ASSERT_EQ(strstr(resp, "\nmogbus_control_requests_total{result=\"ok\"} 2\n") != NULL);
	ASSERT_EQ(strstr(resp, "\nmogbus_control_requests_total{result=\"error\"} 2\n") != NULL);
	net_obs_close(&obs);
	arena_release(arena);
}
#endif
