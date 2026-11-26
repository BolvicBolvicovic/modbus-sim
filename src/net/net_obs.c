/**
 * @file
 * @brief Request metrics, structured request logs and the HTTP health, readiness and
 *        Prometheus endpoint of one server.
 */
#include "net/net_obs.h"
#include "mogbus.h"
#include "net/net_common.h"
#include "net/net_control.h"
#include "os/os_time.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const u32 NET_OBS_LATENCY_MS[NET_OBS_LATENCY_BUCKETS] = {
	1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000,
};

static const char* const RESULT_NAMES[NET_RESULT_COUNT] = {
	[NET_RESULT_OK]        = "ok",
	[NET_RESULT_EXCEPTION] = "exception",
	[NET_RESULT_NONE]      = "none",
	[NET_RESULT_DROPPED]   = "dropped",
};

s32
net_obs_init(net_obs_t* obs, const char* transport, device_t** devices, u32 device_count,
	     const char* http_bind, u16 http_port)
{
	memset(obs, 0, sizeof(*obs));
	obs->transport    = transport;
	obs->devices      = devices;
	obs->device_count = MIN(device_count, (u32)NET_OBS_MAX_DEVICES);
	obs->start_unix_s = (u64)time(NULL);
	obs->http.listen_fd = OS_SOCKET_INVALID;
	for (u32 i = 0; i < NET_OBS_HTTP_CONNS; i++)
		obs->http.conns[i].fd = OS_SOCKET_INVALID;
	if (!http_bind)
		return 0;
	obs->http.listen_fd = net_tcp_listen(http_bind, http_port, 16);
	if (obs->http.listen_fd == OS_SOCKET_INVALID)
	{
		LOG_WARN("cannot listen for HTTP on %s:%u", http_bind, http_port);
		return -1;
	}
	LOG_INFO("HTTP endpoint on %s:%u (/healthz, /readyz, /metrics)", http_bind, http_port);
	return 0;
}

void
net_obs_close(net_obs_t* obs)
{
	if (!obs)
		return;
	for (u32 i = 0; i < NET_OBS_HTTP_CONNS; i++)
	{
		if (obs->http.conns[i].fd >= 0)
			os_socket_close(obs->http.conns[i].fd);
		obs->http.conns[i].fd = OS_SOCKET_INVALID;
	}
	if (obs->http.listen_fd >= 0)
		os_socket_close(obs->http.listen_fd);
	obs->http.listen_fd = OS_SOCKET_INVALID;
	obs->ready = false;
}

static s32
device_index(const net_obs_t* obs, const device_t* dev)
{
	for (u32 i = 0; dev && i < obs->device_count; i++)
	{
		if (obs->devices[i] == dev)
			return (s32)i;
	}
	return -1;
}

void
net_obs_request(net_obs_t* obs, const char* peer, const device_t* dev, u8 unit, u8 fc,
		const u8* resp, u32 resp_len, const fault_decision_t* decision)
{
	static const fault_decision_t	none = { 0 };
	const fault_decision_t*		d = decision ? decision : &none;
	bool				fired[FAULT_KIND_COUNT] = {
		[FAULT_DELAY]             = d->delay_ms > 0,
		[FAULT_DROP]              = d->drop,
		[FAULT_EXCEPTION]         = d->exception_code != 0,
		[FAULT_CORRUPT]           = d->corrupt != FAULT_CORRUPT_NONE,
		[FAULT_DISCONNECT]        = d->disconnect,
		[FAULT_RESTART]           = d->restart,
		[FAULT_SENSOR_DISCONNECT] = d->sensor_disconnect,
	};
	net_result_t			result;
	u8				code = 0;
	s32				idx;

	if (d->drop || d->disconnect)
		result = NET_RESULT_DROPPED;
	else if (resp_len == 0)
		result = NET_RESULT_NONE;
	else if (resp[0] & 0x80)
	{
		result = NET_RESULT_EXCEPTION;
		code   = resp_len > 1 ? resp[1] : 0;
	}
	else
		result = NET_RESULT_OK;

	if (obs)
	{
		struct net_metrics_t* m = &obs->metrics;

		m->requests_by_fc[fc]++;
		m->responses[result]++;
		if (result == NET_RESULT_OK || result == NET_RESULT_EXCEPTION)
		{
			u32 b = 0;

			while (b < NET_OBS_LATENCY_BUCKETS && d->delay_ms > NET_OBS_LATENCY_MS[b])
				b++;
			if (b < NET_OBS_LATENCY_BUCKETS)
				m->latency_bucket[b]++;
			m->latency_count++;
			m->latency_sum_ms += d->delay_ms;
		}
		idx = device_index(obs, dev);
		if (idx >= 0)
		{
			struct net_device_metrics_t* dm = &m->device[idx];

			dm->requests++;
			dm->responses[result]++;
			if (result == NET_RESULT_EXCEPTION)
				dm->exceptions[code & 0x0F]++;
			for (u32 k = 0; k < FAULT_KIND_COUNT; k++)
				dm->faults[k] += fired[k];
		}
	}

	if (!obs || !log_enabled(LOG_LEVEL_DEBUG))
		return;
	{
		const char*	transport = obs->transport ? obs->transport : "modbus";
		char		faults[128] = "";
		char		text_faults[128] = "";
		char		peer_json[96] = "";
		char		fields[512];
		u32		fl = 0;
		u32		tl = 0;

		for (u32 k = 0; k < FAULT_KIND_COUNT; k++)
		{
			if (!fired[k])
				continue;
			fl += (u32)snprintf(faults + fl, sizeof(faults) - fl, "%s\"%s\"",
					    fl ? "," : "", fault_kind_name((fault_kind_t)k));
			tl += (u32)snprintf(text_faults + tl, sizeof(text_faults) - tl, "%s%s",
					    tl ? "," : " (fault: ", fault_kind_name((fault_kind_t)k));
		}
		if (tl)
			snprintf(text_faults + tl, sizeof(text_faults) - tl, ")");
		if (peer)
			log_json_escape(peer_json, sizeof(peer_json), peer);
		snprintf(fields, sizeof(fields),
			 "\"event\":\"request\",\"transport\":\"%s\"%s%s%s,\"unit\":%u,\"fc\":%u,"
			 "\"result\":\"%s\",\"exception\":%u,\"latency_ms\":%u,\"faults\":[%s]",
			 transport, peer ? ",\"peer\":\"" : "", peer ? peer_json : "", peer ? "\"" : "",
			 unit, fc, RESULT_NAMES[result], code, d->delay_ms, faults);
		{
			char code_text[8] = "";

			if (result == NET_RESULT_EXCEPTION)
				snprintf(code_text, sizeof(code_text), " 0x%02X", code);
			log_write_fields(LOG_LEVEL_DEBUG, fields, "%s%s%s unit=%u fc=0x%02X -> %s%s%s",
					 transport, peer ? " " : "", peer ? peer : "", unit, fc,
					 RESULT_NAMES[result], code_text, text_faults);
		}
	}
}

void
net_obs_rejected(net_obs_t* obs)
{
	if (obs)
		obs->metrics.frames_rejected++;
}

void
net_obs_connection(net_obs_t* obs, bool opened)
{
	if (!obs)
		return;
	if (opened)
	{
		obs->metrics.connections_total++;
		obs->metrics.connections_active++;
	}
	else if (obs->metrics.connections_active > 0)
		obs->metrics.connections_active--;
}

/* Appends to a bounded buffer; `*used` past `size` marks an overflow. */
static void __attribute__((format(printf, 4, 5)))
put(char* out, u32 size, u32* used, const char* fmt, ...)
{
	va_list	ap;
	int	n;

	if (*used >= size)
		return;
	va_start(ap, fmt);
	n = vsnprintf(out + *used, size - *used, fmt, ap);
	va_end(ap);
	*used = n < 0 ? size : *used + (u32)n;
}

/* Prometheus label values escape backslash, quote and newline. */
static void
label_escape(char* out, u32 size, const char* text)
{
	u32 used = 0;

	for (const char* p = text ? text : ""; *p && used + 2 < size; p++)
	{
		if (*p == '\\' || *p == '"' || *p == '\n')
		{
			out[used++] = '\\';
			out[used++] = *p == '\n' ? 'n' : *p;
		}
		else
			out[used++] = *p;
	}
	out[used] = '\0';
}

u32
net_obs_render_metrics(const net_obs_t* obs, char* out, u32 size)
{
	const struct net_metrics_t*	m = &obs->metrics;
	u32				used = 0;
	u64				cumulative = 0;

	put(out, size, &used, "# HELP mogbus_info Version and transport of this server.\n"
			      "# TYPE mogbus_info gauge\n"
			      "mogbus_info{version=\"%d.%d.%d\",transport=\"%s\"} 1\n",
	    MOGBUS_VERSION_MAJOR, MOGBUS_VERSION_MINOR, MOGBUS_VERSION_PATCH,
	    obs->transport ? obs->transport : "");
	put(out, size, &used, "# HELP mogbus_ready 1 while the server accepts requests.\n"
			      "# TYPE mogbus_ready gauge\nmogbus_ready %d\n", obs->ready ? 1 : 0);
	put(out, size, &used, "# HELP mogbus_start_time_seconds Server start time, Unix seconds.\n"
			      "# TYPE mogbus_start_time_seconds gauge\nmogbus_start_time_seconds %llu\n",
	    (unsigned long long)obs->start_unix_s);

	put(out, size, &used, "# HELP mogbus_requests_total Decoded requests, by function code.\n"
			      "# TYPE mogbus_requests_total counter\n");
	for (u32 fc = 0; fc < 256; fc++)
	{
		if (m->requests_by_fc[fc])
			put(out, size, &used, "mogbus_requests_total{fc=\"%u\"} %llu\n", fc,
			    (unsigned long long)m->requests_by_fc[fc]);
	}
	put(out, size, &used, "# HELP mogbus_responses_total Requests by outcome: ok, exception, none "
			      "(no response by protocol), dropped (by a fault).\n"
			      "# TYPE mogbus_responses_total counter\n");
	for (u32 r = 0; r < NET_RESULT_COUNT; r++)
		put(out, size, &used, "mogbus_responses_total{result=\"%s\"} %llu\n", RESULT_NAMES[r],
		    (unsigned long long)m->responses[r]);
	put(out, size, &used, "# HELP mogbus_frames_rejected_total Frames rejected before reaching a "
			      "device (malformed, bad checksum, oversized).\n"
			      "# TYPE mogbus_frames_rejected_total counter\nmogbus_frames_rejected_total %llu\n",
	    (unsigned long long)m->frames_rejected);

	put(out, size, &used, "# HELP mogbus_response_latency_seconds Time from a request to its "
			      "scheduled response, including delay faults.\n"
			      "# TYPE mogbus_response_latency_seconds histogram\n");
	for (u32 b = 0; b < NET_OBS_LATENCY_BUCKETS; b++)
	{
		cumulative += m->latency_bucket[b];
		put(out, size, &used, "mogbus_response_latency_seconds_bucket{le=\"%g\"} %llu\n",
		    NET_OBS_LATENCY_MS[b] / 1000.0, (unsigned long long)cumulative);
	}
	put(out, size, &used, "mogbus_response_latency_seconds_bucket{le=\"+Inf\"} %llu\n"
			      "mogbus_response_latency_seconds_sum %g\n"
			      "mogbus_response_latency_seconds_count %llu\n",
	    (unsigned long long)m->latency_count, m->latency_sum_ms / 1000.0,
	    (unsigned long long)m->latency_count);

	put(out, size, &used, "# HELP mogbus_connections_active Open client connections (TCP modes).\n"
			      "# TYPE mogbus_connections_active gauge\nmogbus_connections_active %u\n"
			      "# HELP mogbus_connections_total Accepted client connections (TCP modes).\n"
			      "# TYPE mogbus_connections_total counter\nmogbus_connections_total %llu\n",
	    m->connections_active, (unsigned long long)m->connections_total);

	if (obs->control.enabled)
		put(out, size, &used, "# HELP mogbus_control_requests_total Runtime control API requests.\n"
				      "# TYPE mogbus_control_requests_total counter\n"
				      "mogbus_control_requests_total{result=\"ok\"} %llu\n"
				      "mogbus_control_requests_total{result=\"error\"} %llu\n",
		    (unsigned long long)m->control_requests[0], (unsigned long long)m->control_requests[1]);

	put(out, size, &used, "# HELP mogbus_device_info Configured devices.\n# TYPE mogbus_device_info gauge\n");
	for (u32 i = 0; i < obs->device_count; i++)
	{
		char name[160];

		label_escape(name, sizeof(name), obs->devices[i]->name);
		put(out, size, &used, "mogbus_device_info{unit=\"%u\",name=\"%s\"} 1\n",
		    obs->devices[i]->unit_id, name);
	}
	put(out, size, &used, "# HELP mogbus_device_requests_total Requests addressed to a device.\n"
			      "# TYPE mogbus_device_requests_total counter\n");
	for (u32 i = 0; i < obs->device_count; i++)
		put(out, size, &used, "mogbus_device_requests_total{unit=\"%u\"} %llu\n",
		    obs->devices[i]->unit_id, (unsigned long long)m->device[i].requests);
	put(out, size, &used, "# HELP mogbus_device_responses_total Device requests by outcome.\n"
			      "# TYPE mogbus_device_responses_total counter\n");
	for (u32 i = 0; i < obs->device_count; i++)
	{
		for (u32 r = 0; r < NET_RESULT_COUNT; r++)
			put(out, size, &used, "mogbus_device_responses_total{unit=\"%u\",result=\"%s\"} %llu\n",
			    obs->devices[i]->unit_id, RESULT_NAMES[r],
			    (unsigned long long)m->device[i].responses[r]);
	}
	put(out, size, &used, "# HELP mogbus_device_exceptions_total Exception responses, by code.\n"
			      "# TYPE mogbus_device_exceptions_total counter\n");
	for (u32 i = 0; i < obs->device_count; i++)
	{
		for (u32 c = 0; c < 16; c++)
		{
			if (m->device[i].exceptions[c])
				put(out, size, &used, "mogbus_device_exceptions_total{unit=\"%u\",code=\"%u\"} %llu\n",
				    obs->devices[i]->unit_id, c, (unsigned long long)m->device[i].exceptions[c]);
		}
	}
	put(out, size, &used, "# HELP mogbus_device_faults_total Injected faults that fired, by kind.\n"
			      "# TYPE mogbus_device_faults_total counter\n");
	for (u32 i = 0; i < obs->device_count; i++)
	{
		for (u32 k = 0; k < FAULT_KIND_COUNT; k++)
		{
			if (obs->devices[i]->faults.present & FAULT_BIT(k) || m->device[i].faults[k])
				put(out, size, &used, "mogbus_device_faults_total{unit=\"%u\",kind=\"%s\"} %llu\n",
				    obs->devices[i]->unit_id, fault_kind_name((fault_kind_t)k),
				    (unsigned long long)m->device[i].faults[k]);
		}
	}
	put(out, size, &used, "# HELP mogbus_device_comm_errors_total Bus communication errors seen by a "
			      "device (FC 08 0x000C, 16-bit, wraps).\n"
			      "# TYPE mogbus_device_comm_errors_total counter\n");
	for (u32 i = 0; i < obs->device_count; i++)
		put(out, size, &used, "mogbus_device_comm_errors_total{unit=\"%u\"} %u\n",
		    obs->devices[i]->unit_id, obs->devices[i]->diag_comm_error_count);
	return used < size ? used : 0;
}

static void
http_close(struct net_http_conn_t* conn)
{
	if (conn->fd >= 0)
		os_socket_close(conn->fd);
	conn->fd = OS_SOCKET_INVALID;
}

/* Builds the whole response into conn->out: the body is rendered behind room for the head,
 * then the head is written and the body moved up against it. `forced` is a status for a
 * request that cannot be parsed (400, 413); 0 routes it. */
static void
http_respond(net_obs_t* obs, struct net_http_conn_t* conn, int forced)
{
	enum { HEAD_ROOM = 256 };
	char*		body = conn->out + HEAD_ROOM;
	u32		cap = sizeof(conn->out) - HEAD_ROOM;
	char		method[8] = "";
	char		path[128] = "";
	char		head[HEAD_ROOM];
	const char*	status = "200 OK";
	const char*	type = "text/plain; charset=utf-8";
	const char*	query = "";
	bool		is_head;
	bool		api;
	u32		body_len;
	int		code = 200;
	int		n;

	sscanf(conn->in, "%7s %127s", method, path);
	if (path[strcspn(path, "?")] == '?')
		query = path + strcspn(path, "?") + 1;
	path[strcspn(path, "?")] = '\0';
	is_head = strcmp(method, "HEAD") == 0;
	api = strncmp(path, "/api/", 5) == 0;
	if (api && !obs->control.enabled)
		code = 404, body_len = (u32)snprintf(body, cap, "not found; try /healthz, /readyz or /metrics\n");
	else if (forced != 0 || api)
	{
		if (forced != 0)
		{
			code     = forced;
			body_len = (u32)snprintf(body, cap, "{\"error\":\"%s\"}",
						 forced == 413 ? "request body too large" : "bad Content-Length");
		}
		else
		{
			conn->in[conn->head_len + conn->body_len] = '\0';
			body_len = net_control_handle(obs, method, path, query, conn->in + conn->head_len,
						      body, cap, &code);
		}
		type = "application/json";
		obs->metrics.control_requests[code >= 400]++;
	}
	else if (strcmp(method, "GET") != 0 && !is_head)
		code = 405, body_len = (u32)snprintf(body, cap, "only GET and HEAD\n");
	else if (strcmp(path, "/healthz") == 0)
		body_len = (u32)snprintf(body, cap, "ok\n");
	else if (strcmp(path, "/readyz") == 0)
	{
		if (!obs->ready)
			code = 503;
		body_len = (u32)snprintf(body, cap, obs->ready ? "ready\n" : "not ready\n");
	}
	else if (strcmp(path, "/metrics") == 0)
	{
		type     = "text/plain; version=0.0.4; charset=utf-8";
		body_len = net_obs_render_metrics(obs, body, cap);
		if (body_len == 0)
		{
			code     = 500;
			body_len = (u32)snprintf(body, cap, "metrics too large\n");
		}
	}
	else
		code = 404, body_len = (u32)snprintf(body, cap, "not found; try /healthz, /readyz or /metrics\n");
	switch (code)
	{
		case 400: status = "400 Bad Request"; break;
		case 404: status = "404 Not Found"; break;
		case 405: status = "405 Method Not Allowed"; break;
		case 413: status = "413 Payload Too Large"; break;
		case 422: status = "422 Unprocessable Entity"; break;
		case 500: status = "500 Internal Server Error"; break;
		case 503: status = "503 Service Unavailable"; break;
		default: break;
	}
	n = snprintf(head, sizeof(head),
		     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
		     status, type, body_len);
	if (n < 0 || (u32)n >= sizeof(head))
	{
		http_close(conn);
		return;
	}
	if (is_head)
		body_len = 0;
	memmove(conn->out + n, body, body_len);
	memcpy(conn->out, head, (u32)n);
	conn->out_len  = (u32)n + body_len;
	conn->out_sent = 0;
	conn->writing  = true;
}

bool
net_obs_http_feed(net_obs_t* obs, struct net_http_conn_t* conn)
{
	conn->in[conn->in_len] = '\0';
	if (conn->head_len == 0)
	{
		const char*	crlf = strstr(conn->in, "\r\n\r\n");
		const char*	lf   = strstr(conn->in, "\n\n");
		const char*	line;
		bool		bad_length = false;

		if (!crlf && !lf)
			return conn->in_len < NET_OBS_HTTP_IN;	/* else the head is too large */
		if (crlf && (!lf || crlf < lf))
			conn->head_len = (u32)(crlf - conn->in) + 4;
		else
			conn->head_len = (u32)(lf - conn->in) + 2;
		if (conn->head_len > NET_OBS_HTTP_IN)
			return false;
		/* Header lines start after the request line. */
		for (line = strchr(conn->in, '\n'); line && line < conn->in + conn->head_len;
		     line = strchr(line + 1, '\n'))
		{
			const char* v = line + 1;
			u64	    len = 0;
			u32	    k = 0;

			while (k < 15 && tolower((unsigned char)v[k]) == "content-length:"[k])
				k++;
			if (k != 15)
				continue;
			v += 15;
			while (*v == ' ' || *v == '\t')
				v++;
			bad_length = !(*v >= '0' && *v <= '9');
			for (; *v >= '0' && *v <= '9' && len < 0xFFFFFFF; v++)
				len = len * 10 + (u64)(*v - '0');
			bad_length = bad_length || (*v != '\r' && *v != '\n' && *v != ' ' && *v != '\t');
			conn->body_len = (u32)len;
		}
		if (bad_length)
		{
			http_respond(obs, conn, 400);
			return true;
		}
		if (conn->body_len > NET_OBS_HTTP_BODY)
		{
			http_respond(obs, conn, 413);
			return true;
		}
	}
	if (conn->in_len >= conn->head_len + conn->body_len)
		http_respond(obs, conn, 0);
	return true;
}

static void
http_read(net_obs_t* obs, struct net_http_conn_t* conn)
{
	s64 got = os_socket_recv(conn->fd, conn->in + conn->in_len, sizeof(conn->in) - 1 - conn->in_len);

	if (got < 0 && os_retry())
		return;
	if (got <= 0)
	{
		http_close(conn);
		return;
	}
	conn->in_len += (u32)got;
	if (!net_obs_http_feed(obs, conn))
		http_close(conn);
}

static void
http_write(struct net_http_conn_t* conn)
{
	s64 sent = os_socket_send(conn->fd, conn->out + conn->out_sent, conn->out_len - conn->out_sent);

	if (sent < 0 && os_retry())
		return;
	if (sent <= 0)
	{
		http_close(conn);
		return;
	}
	conn->out_sent += (u32)sent;
	if (conn->out_sent == conn->out_len)
		http_close(conn);
}

int
net_poll(net_obs_t* obs, os_wait_entry_t* fds, u32 count, int timeout_ms)
{
	os_wait_entry_t			all[NET_OBS_POLL_MAX];
	struct net_http_conn_t*		owner[NET_OBS_POLL_MAX];
	struct net_http_t*		http = obs ? &obs->http : NULL;
	u64				now_ms = os_now_ms();
	u32				n = count;
	bool				slot_free = false;
	int				rc;

	if (!http || http->listen_fd < 0)
		return os_wait(fds, count, timeout_ms);
	if (count)
		memcpy(all, fds, count * sizeof(*fds));
	for (u32 i = 0; i < NET_OBS_HTTP_CONNS && n < NET_OBS_POLL_MAX; i++)
	{
		struct net_http_conn_t* conn = &http->conns[i];

		if (conn->fd < 0)
		{
			slot_free = true;
			continue;
		}
		if (conn->deadline_ms <= now_ms)
		{
			http_close(conn);
			slot_free = true;
			continue;
		}
		owner[n] = conn;
		all[n++] = (os_wait_entry_t){ .fd = conn->fd, .events = conn->writing ? OS_WAIT_OUT : OS_WAIT_IN };
		if (timeout_ms < 0 || (u64)timeout_ms > conn->deadline_ms - now_ms)
			timeout_ms = (int)(conn->deadline_ms - now_ms);
	}
	if (slot_free && n < NET_OBS_POLL_MAX)
	{
		owner[n] = NULL;
		all[n++] = (os_wait_entry_t){ .fd = http->listen_fd, .events = OS_WAIT_IN };
	}

	rc = os_wait(all, n, timeout_ms);
	for (u32 i = 0; i < count; i++)
		fds[i].revents = rc > 0 ? all[i].revents : 0;
	if (rc <= 0)
		return rc;

	for (u32 i = count; i < n; i++)
	{
		struct net_http_conn_t* conn = owner[i];

		if (!all[i].revents)
			continue;
		if (!conn)
		{
			for (u32 s = 0; s < NET_OBS_HTTP_CONNS; s++)
			{
				os_addr_t	peer;
				os_socket_t	fd;

				if (http->conns[s].fd >= 0)
					continue;
				fd = os_tcp_accept(http->listen_fd, &peer);
				if (fd == OS_SOCKET_INVALID)
					break;
				memset(&http->conns[s], 0, offsetof(struct net_http_conn_t, in));
				http->conns[s].fd          = fd;
				http->conns[s].deadline_ms = now_ms + NET_OBS_HTTP_TIMEOUT_MS;
				http->conns[s].in[0]       = '\0';
			}
			continue;
		}
		if (all[i].revents & (OS_WAIT_ERR | OS_WAIT_INVAL))
			http_close(conn);
		else if (conn->writing)
			http_write(conn);
		else
			http_read(obs, conn);
	}
	return rc;
}
