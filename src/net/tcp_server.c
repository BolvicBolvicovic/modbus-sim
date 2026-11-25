#define _GNU_SOURCE
#include "net/tcp_server.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
#include "net/net_tcp_pool.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
conn_close(tcp_server_t* server, tcp_conn_t* conn)
{
	net_tcp_pool_close_conn(&server->pool, &conn->base);
}

static bool
conn_tls_handshake(tcp_server_t* server, tcp_conn_t* conn)
{
	char			tls_err[256];
	char			subject[512];
	net_tls_status_t	status;

	status = net_tls_conn_handshake(conn->tls, tls_err, sizeof(tls_err));
	if (status == NET_TLS_OK)
	{
		conn->base.last_activity_ms = clock_now_ms();
		LOG_DEBUG("TLS handshake complete with %s", conn->base.peer);
		if (net_tls_conn_peer_subject(conn->tls, subject, sizeof(subject)))
			LOG_INFO("client %s authenticated TLS peer subject: %s", conn->base.peer, subject);
		return true;
	}
	if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
		return true;
	if (status == NET_TLS_ERROR)
		LOG_WARN("client %s TLS handshake failed: %s", conn->base.peer, tls_err);
	conn_close(server, conn);
	return false;
}

static void
conn_on_close(void* ctx, net_tcp_conn_t* base)
{
	tcp_conn_t* conn = (tcp_conn_t*)base;

	(void)ctx;
	if (conn->tls)
	{
		net_tls_conn_close(conn->tls);
		conn->tls = NULL;
	}
	conn->rx_len = 0;
	conn->tx_len = 0;
}

static void
conn_on_accept(void* ctx, net_tcp_conn_t* base, u64 now_ms)
{
	tcp_server_t*	server = ctx;
	tcp_conn_t*		conn   = (tcp_conn_t*)base;
	char		tls_err[256];
	u64		deadline_ms = 0;

	if (server->options.tls_handshake_timeout_ms > 0)
		deadline_ms = now_ms + server->options.tls_handshake_timeout_ms;
	if (server->tls.ssl_ctx)
	{
		conn->tls = net_tls_conn_new(&server->tls, base->fd, deadline_ms,
					     tls_err, sizeof(tls_err));
		if (!conn->tls)
		{
			LOG_WARN("client %s TLS setup failed: %s", base->peer, tls_err);
			conn_close(server, conn);
			return;
		}
		(void)conn_tls_handshake(server, conn);
	}
}

static bool
conn_skip_idle(void* ctx, const net_tcp_conn_t* base)
{
	const tcp_conn_t* conn = (const tcp_conn_t*)base;

	(void)ctx;
	return conn->tx_len > 0;
}

/* Removes the first `size` bytes of the receive buffer. */
static void
conn_consume(tcp_conn_t* conn, u32 size)
{
	memmove(conn->rx, conn->rx + size, conn->rx_len - size);
	conn->rx_len -= size;
}

/* Runs the request found at the start of conn->rx and queues the response (if any). */
static void
handle_frame(tcp_server_t* server, tcp_conn_t* conn, const mbap_header_t* header, u64 now_ms)
{
	u8			pdu[MODBUS_MAX_PDU];
	device_t*		dev      = net_find_device(server->options.devices,
							   server->options.device_count,
							   header->unit);
	const u8*		request  = conn->rx + MBAP_HEADER_SIZE;
	u32			req_len  = (u32)header->length - 1;
	u32			pdu_len  = 0;
	fault_decision_t	decision = { 0 };
	mbap_header_t		reply    = *header;

	if (!dev)
		pdu_len = modbus_exception(pdu, request[0], EXC_GATEWAY_TARGET_FAILED);
	else
		pdu_len = net_handle_pdu(dev, &server->rng, now_ms, request, req_len, pdu, &decision);

	LOG_DEBUG("%s tid=%u unit=%u fc=0x%02X -> %s%s", conn->base.peer, header->transaction,
		  header->unit, request[0], pdu_len == 0 ? "no response" :
		  (pdu[0] & 0x80) ? "exception" : "ok",
		  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");

	if (decision.disconnect)
	{
		conn_close(server, conn);
		return;
	}
	if (decision.drop || pdu_len == 0)
	{
		conn->base.last_activity_ms = now_ms;
		return;
	}

	reply.length = (u16)(pdu_len + 1);
	mbap_encode(conn->tx, &reply);
	memcpy(conn->tx + MBAP_HEADER_SIZE, pdu, pdu_len);
	conn->tx_len           = MBAP_HEADER_SIZE + pdu_len;
	conn->tx_sent          = 0;
	conn->tx_due_ms        = now_ms + decision.delay_ms;
	conn->base.last_activity_ms = now_ms;
}

/* Handles every complete request waiting in the receive buffer, one at a time. */
static void
conn_process(tcp_server_t* server, tcp_conn_t* conn)
{
	mbap_header_t	header = { 0 };
	u32		frame  = 0;

	while (conn->base.fd >= 0 && conn->tx_len == 0 && conn->rx_len >= MBAP_HEADER_SIZE)
	{
		mbap_decode(conn->rx, &header);
		if (!mbap_is_valid(&header))
		{
			LOG_WARN("client %s sent a bad MBAP header, closing", conn->base.peer);
			conn_close(server, conn);
			return;
		}

		frame = mbap_frame_size(&header);
		if (conn->rx_len < frame)
			return;

		handle_frame(server, conn, &header, clock_now_ms());
		if (conn->base.fd < 0)
			return; /* a fault closed the connection */
		conn_consume(conn, frame);
	}
}

static void
conn_tx_advance(tcp_server_t* server, tcp_conn_t* conn, u32 sent)
{
	conn->tx_sent         += sent;
	conn->base.last_activity_ms = clock_now_ms();
	if (conn->tx_sent < conn->tx_len)
		return;

	conn->tx_len  = 0;
	conn->tx_sent = 0;
	conn_process(server, conn);	/* a pipelined request may already be waiting */
}

static void
conn_read(tcp_server_t* server, tcp_conn_t* conn)
{
	ssize_t	got;

	if (conn->tls)
	{
		char			tls_err[256];
		int			len      = 0;
		net_tls_status_t	status;

		if (conn->rx_len == sizeof(conn->rx))
		{
			LOG_WARN("client %s receive buffer full, closing", conn->base.peer);
			conn_close(server, conn);
			return;
		}

		status = net_tls_conn_read(conn->tls, conn->rx + conn->rx_len,
					   (int)(sizeof(conn->rx) - conn->rx_len), &len,
					   tls_err, sizeof(tls_err));
		if (status == NET_TLS_OK && len > 0)
		{
			conn->rx_len          += (u32)len;
			conn->base.last_activity_ms = clock_now_ms();
			conn_process(server, conn);
			return;
		}
		if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
			return;
		if (status == NET_TLS_ERROR && len == 0)
			LOG_WARN("client %s TLS read failed: %s", conn->base.peer, tls_err);
		conn_close(server, conn);
		return;
	}

	got = recv(conn->base.fd, conn->rx + conn->rx_len, sizeof(conn->rx) - conn->rx_len, 0);
	if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR))
	{
		conn_close(server, conn);
		return;
	}
	if (got < 0)
		return;

	conn->rx_len          += (u32)got;
	conn->base.last_activity_ms = clock_now_ms();
	conn_process(server, conn);
}

static void
conn_write(tcp_server_t* server, tcp_conn_t* conn)
{
	ssize_t	sent;

	if (conn->tls)
	{
		char			tls_err[256];
		int			len      = 0;
		net_tls_status_t	status;

		status = net_tls_conn_write(conn->tls, conn->tx + conn->tx_sent,
					    (int)(conn->tx_len - conn->tx_sent), &len,
					    tls_err, sizeof(tls_err));
		if (status == NET_TLS_OK && len > 0)
		{
			conn_tx_advance(server, conn, (u32)len);
			return;
		}
		if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
			return;
		if (status == NET_TLS_ERROR && len == 0)
			LOG_WARN("client %s TLS write failed: %s", conn->base.peer, tls_err);
		conn_close(server, conn);
		return;
	}

	sent = send(conn->base.fd, conn->tx + conn->tx_sent, conn->tx_len - conn->tx_sent,
		    MSG_NOSIGNAL);
	if (sent < 0 &&
	    errno != EAGAIN &&
	    errno != EINTR)
	{
		conn_close(server, conn);
		return;
	}
	if (sent < 0)
		return;

	conn_tx_advance(server, conn, (u32)sent);
}

/* --- main loop --- */

/* A TLS handshake has its own deadline from accept(), even with idle timeout disabled. */
static void
close_stalled_handshakes(tcp_server_t* server, u64 now_ms)
{
	u32 timeout_ms = server->options.tls_handshake_timeout_ms;

	if (!server->tls.ssl_ctx || timeout_ms == 0)
		return;

	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		tcp_conn_t*	conn     = &server->conns[i];
		u64	deadline = net_tls_conn_handshake_deadline(conn->tls);

		if (conn->base.fd < 0 ||
		    !conn->tls ||
		    net_tls_conn_handshake_done(conn->tls) ||
		    deadline == 0 ||
		    now_ms < deadline)
			continue;

		LOG_WARN("client %s TLS handshake exceeded %u ms, closing", conn->base.peer, timeout_ms);
		conn_close(server, conn);
	}
}

/* OpenSSL may hold decrypted request bytes even when the socket has nothing to read.
 * Only synthesize a wakeup while no response is pending, so delayed faults are honored. */
static bool
conn_tls_has_buffered_input(const tcp_conn_t* conn)
{
	return conn->tls &&
	       net_tls_conn_handshake_done(conn->tls) &&
	       conn->tx_len == 0 &&
	       net_tls_conn_pending(conn->tls) > 0;
}

/* Fills the poll table. Returns the number of entries; entry 0 is the listening socket. */
static u32
build_poll_table(tcp_server_t* server, struct pollfd* fds, net_tcp_conn_t** owners,
		 u64 now_ms, int* timeout)
{
	u32 count = net_tcp_pool_build_poll(&server->pool, fds, owners, now_ms, timeout);

	for (u32 i = 1; i < count; i++)
	{
		tcp_conn_t*	conn         = (tcp_conn_t*)owners[i];
		short	tls_events   = conn->tls ? net_tls_conn_poll_events(conn->tls) : 0;
		u64	handshake_deadline;
		short	events = POLLIN;

		if (conn->tls && !net_tls_conn_handshake_done(conn->tls))
		{
			events = tls_events;
			handshake_deadline = net_tls_conn_handshake_deadline(conn->tls);
			if (handshake_deadline > 0)
			{
				if (handshake_deadline <= now_ms)
					*timeout = 0;
				else
					*timeout = MIN(*timeout, (int)(handshake_deadline - now_ms));
			}
		}
		else if (conn->tls && conn->tx_len == 0 && (tls_events & POLLOUT))
			events = POLLOUT;

		/* Decrypted bytes may already be buffered inside OpenSSL. */
		if (conn_tls_has_buffered_input(conn))
		{
			events |= POLLIN;
			*timeout = 0;
		}

		if (conn->tx_len == 0)
		{
			fds[i].events = events;
			continue;
		}

		/* A response is pending: wait for its due time, then for the socket to be writable. */
		if (conn->tx_due_ms > now_ms)
			*timeout = MIN(*timeout, (int)(conn->tx_due_ms - now_ms));
		if (conn->tls)
		{
			events = tls_events;
			if (events == 0 && conn->tx_due_ms <= now_ms)
				events = POLLOUT;
		}
		else
			events = (conn->tx_due_ms <= now_ms) ? POLLOUT : 0;
		fds[i].events = events;
	}
	return count;
}

static void
serve_ready(tcp_server_t* server, const struct pollfd* fd, net_tcp_conn_t* owner, u64 now_ms)
{
	tcp_conn_t*	conn          = (tcp_conn_t*)owner;
	bool	buffered_input = owner && conn_tls_has_buffered_input(conn);
	short	tls_events;

	if (fd->revents == 0 && !buffered_input)
		return;

	if (!owner)
	{
		net_tcp_pool_accept(&server->pool);
		return;
	}

	if (conn->base.fd < 0 || fd->fd != conn->base.fd)
		return;

	if (fd->revents & (POLLERR | POLLNVAL))
	{
		conn_close(server, conn);
		return;
	}

	if (conn->tls && !net_tls_conn_handshake_done(conn->tls))
	{
		if (!conn_tls_handshake(server, conn) || !net_tls_conn_handshake_done(conn->tls))
			return;
		if ((fd->revents & (POLLIN | POLLHUP)) != 0 ||
		    conn_tls_has_buffered_input(conn))
			conn_read(server, conn);
		return;
	}

	tls_events = conn->tls ? net_tls_conn_poll_events(conn->tls) : 0;

	/* WANT_WRITE from SSL_read must be retried as a read, never as an empty write. */
	if (conn->tls && conn->tx_len == 0 && tls_events != 0)
	{
		conn_read(server, conn);
		return;
	}

	/* A TLS write can need a read (renegotiation/post-handshake messages), but
	 * a delayed response must never be sent early. Do not consume new request
	 * bytes while a response is still pending. */
	if (conn->tls && conn->tx_len > 0)
	{
		/* poll reports POLLHUP even when events is 0: a disconnect must free
		 * the slot without waiting for the delayed response to become due. */
		if (fd->revents & POLLHUP)
		{
			conn_close(server, conn);
			return;
		}
		if (tls_events != 0 || conn->tx_due_ms <= now_ms)
			conn_write(server, conn);
		return;
	}

	if (fd->revents & POLLOUT)
	{
		conn_write(server, conn);
		return;
	}
	if ((fd->revents & (POLLIN | POLLHUP)) != 0 || buffered_input)
		conn_read(server, conn);
}

s32
tcp_server_step(tcp_server_t* server, s32 max_wait_ms)
{
	struct pollfd		fds[TCP_SERVER_MAX_CONNECTIONS + 1];
	net_tcp_conn_t*		owners[TCP_SERVER_MAX_CONNECTIONS + 1];
	u32		count   = 0;
	int		timeout = 0;
	u64		now_ms  = clock_now_ms();

	if (server->stop)
		return NET_STEP_DONE;

	net_tcp_pool_close_idle(&server->pool, now_ms);
	close_stalled_handshakes(server, now_ms);
	count = build_poll_table(server, fds, owners, now_ms, &timeout);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (poll(fds, count, timeout) < 0)
		return NET_STEP_CONTINUE;	/* EINTR: the caller re-checks stop */

	now_ms = clock_now_ms();
	net_tcp_pool_close_idle(&server->pool, now_ms);
	close_stalled_handshakes(server, now_ms);
	for (u32 i = 0; i < count; i++)
		serve_ready(server, &fds[i], owners[i], now_ms);
	return NET_STEP_CONTINUE;
}

void
tcp_server_stop(tcp_server_t* server)
{
	server->stop = 1;
}

s32
tcp_server_init(tcp_server_t* server, const tcp_server_options_t* options)
{
	char			tls_err[256];

	memset(server, 0, sizeof(*server));
	server->options = *options;
	server->pool.conns = (net_tcp_conn_t*)server->conns;
	server->pool.conn_count = ARRAY_COUNT(server->conns);
	server->pool.conn_size = sizeof(server->conns[0]);
	server->pool.idle_timeout_ms = options->idle_timeout_ms;
	server->pool.listen_fd = -1;
	server->pool.ctx = server;
	server->pool.on_accept = conn_on_accept;
	server->pool.on_close = conn_on_close;
	server->pool.skip_idle = conn_skip_idle;
	rng_seed(&server->rng, options->seed);
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
		server->conns[i].base.fd = -1;

	if (options->tls_enabled)
	{
		if (!net_tls_ctx_init(&server->tls, &options->tls, tls_err, sizeof(tls_err)))
		{
			LOG_WARN("cannot configure TLS: %s", tls_err);
			return 2;
		}
	}

	if (net_tcp_pool_listen(&server->pool, options->bind_address, options->port) < 0)
	{
		tcp_server_close(server);
		return 2;
	}

	LOG_INFO("listening on %s:%u (%u device%s). Ctrl-C to stop.", options->bind_address,
		 options->port, options->device_count, options->device_count == 1 ? "" : "s");
	if (server->tls.ssl_ctx)
		LOG_INFO("TLS enabled: certificate=%s private-key=%s CA=%s client-certificate=%s",
			 options->tls.certificate, options->tls.private_key,
			 options->tls.ca_certificate ? options->tls.ca_certificate : "(none)",
			 options->tls.require_client_certificate ? "required" : "optional");
	return 0;
}

void
tcp_server_close(tcp_server_t* server)
{
	bool was_listening = server->pool.listen_fd >= 0;

	net_tcp_pool_close_all(&server->pool);
	net_tls_ctx_close(&server->tls);
	if (was_listening)
		LOG_INFO("stopped");
}

s32
tcp_server_run(tcp_server_t* server, const tcp_server_options_t* options)
{
	s32 rc = tcp_server_init(server, options);

	if (rc != 0)
		return rc;
	while (tcp_server_step(server, -1) == NET_STEP_CONTINUE)
		;
	tcp_server_close(server);
	return 0;
}
