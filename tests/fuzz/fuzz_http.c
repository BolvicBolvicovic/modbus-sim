/**
 * @file
 * @brief Fuzzes the HTTP request path of the endpoint with the runtime control API enabled:
 *        head and Content-Length parsing, routing, the JSON body reader, and the device and
 *        fault operations, all without sockets.
 *
 * The first input byte picks the transport rules (TCP-like, MBAP); the rest is the request.
 * Invariant: a built response is a well-formed HTTP response whose Content-Length matches
 * its body (HEAD aside), and its length stays inside the connection's output buffer.
 */
#include "fuzz_common.h"
#include "net/net_obs.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	static net_obs_t	obs;
	static bool		ready;
	static device_t*	devices[1];
	struct net_http_conn_t*	conn = &obs.http.conns[0];
	const char*		body;
	unsigned		length = 0;
	char			method[8] = "";

	if (size < 1)
		return 0;
	if (!ready)
	{
		devices[0] = fuzz_device();
		FUZZ_CHECK(net_obs_init(&obs, "tcp", devices, 1, NULL, 0) == 0);
		obs.control.enabled = true;
		ready = true;
	}
	obs.control.tcp_like = data[0] & 1;
	obs.control.mbap     = (data[0] & 2) != 0;
	data++;
	size--;
	if (size > sizeof(conn->in) - 1)
		size = sizeof(conn->in) - 1;
	memset(conn, 0, offsetof(struct net_http_conn_t, in));
	conn->fd = -1;
	memcpy(conn->in, data, size);
	conn->in_len = (u32)size;
	conn->in[size] = '\0';
	/* The feed call may consume conn->in, so read the method first, as the server does. */
	sscanf(conn->in, "%7s", method);
	if (!net_obs_http_feed(&obs, conn) || !conn->writing)
		return 0;
	FUZZ_CHECK(conn->out_len <= sizeof(conn->out));
	FUZZ_CHECK(strncmp(conn->out, "HTTP/1.1 ", 9) == 0);
	body = memmem(conn->out, conn->out_len, "\r\n\r\n", 4);
	FUZZ_CHECK(body != NULL);
	body += 4;
	FUZZ_CHECK(sscanf(strstr(conn->out, "Content-Length: ") + 16, "%u", &length) == 1);
	if (strcmp(method, "HEAD") != 0)
		FUZZ_CHECK(length == (u32)(conn->out + conn->out_len - body));
	return 0;
}
