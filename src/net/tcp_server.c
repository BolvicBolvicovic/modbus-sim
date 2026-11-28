/**
 * @file
 * @brief Modbus TCP (MBAP) server, optionally TLS-wrapped.
 */
#define _GNU_SOURCE
#include "net/tcp_server.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
#include "net/net_obs.h"
#include "net/net_tcp_pool.h"
#include "os/os_time.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <string.h>

static void
conn_close(tcp_server_t* server, tcp_conn_t* conn, net_close_reason_t reason)
{
	net_tcp_pool_close_conn(&server->pool, &conn->base, reason);
}

static bool
conn_tls_handshake(tcp_server_t* server, tcp_conn_t* conn)
{
	char			subject[512];
	net_tls_status_t	status;

	status = net_tls_conn_handshake(conn->tls);
	if (status == NET_TLS_OK)
	{
		conn->base.last_activity_ms = os_now_ms();
		LOG_DEBUG("TLS handshake complete with %s", conn->base.peer);
		if (net_tls_conn_peer_subject(conn->tls, subject, sizeof(subject)))
			LOG_INFO("client %s authenticated TLS peer subject: %s", conn->base.peer, subject);
		return true;
	}
	if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
		return true;
	if (status == NET_TLS_ERROR)
		LOG_WARN("client %s TLS handshake failed: %s", conn->base.peer, conn->tls->error);
	conn_close(server, conn, NET_CLOSE_TLS);
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
	u64		deadline_ms = 0;

	if (server->options.tls_handshake_timeout_ms > 0)
		deadline_ms = now_ms + server->options.tls_handshake_timeout_ms;
	if (server->tls.ssl_ctx)
	{
		conn->tls = net_tls_conn_new(&server->tls, base->fd, deadline_ms);
		if (!conn->tls)
		{
			LOG_WARN("client %s TLS setup failed: %s", base->peer, server->tls.error);
			conn_close(server, conn, NET_CLOSE_TLS);
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

/** @brief Removes the first `size` bytes of the receive buffer. */
static void
conn_consume(tcp_conn_t* conn, u32 size)
{
	memmove(conn->rx, conn->rx + size, conn->rx_len - size);
	conn->rx_len -= size;
}

/** @brief Runs the request found at the start of conn->rx and queues the response (if any). */
static void
handle_frame(tcp_server_t* server, tcp_conn_t* conn, u32 frame_size, u64 now_ms)
{
	mbap_header_t		header   = { 0 };
	u8			frame[MBAP_MAX_FRAME];
	fault_decision_t	decision = { 0 };
	u32			len      = net_frame_rx(server->options.obs, conn->base.peer, conn->rx, frame_size,
						       frame, sizeof(frame));

	conn->base.last_activity_ms = now_ms;
	if (len == 0)
		return;
	/* The hook may have rewritten the frame; the connection is framed by the original header. */
	if (len >= MBAP_HEADER_SIZE)
		mbap_decode(frame, &header);
	if (len < MBAP_HEADER_SIZE + 1 ||
	    !mbap_is_valid(&header) ||
	    mbap_frame_size(&header) != len)
	{
		LOG_DEBUG("client %s frame is malformed after the frame hook (%u bytes), ignored",
			  conn->base.peer, len);
		net_obs_rejected(server->options.obs);
		return;
	}
	len = net_mbap_respond(server->options.obs, server->options.devices, server->options.device_count,
			       &server->rng, now_ms, conn->base.peer, &header, frame + MBAP_HEADER_SIZE,
			       conn->tx, &decision);

	if (decision.disconnect)
	{
		conn_close(server, conn, NET_CLOSE_DISCONNECT_FAULT);
		return;
	}
	if (len == 0)
		return;

	conn->tx_len    = len;
	conn->tx_sent   = 0;
	conn->tx_due_ms = now_ms + decision.delay_ms;
}

/** @brief Handles every complete request waiting in the receive buffer, one at a time. */
static void
conn_process(tcp_server_t* server, tcp_conn_t* conn)
{
	mbap_header_t	header = { 0 };
	u32		frame  = 0;

	while (os_socket_valid(conn->base.fd) && conn->tx_len == 0 && conn->rx_len >= MBAP_HEADER_SIZE)
	{
		mbap_decode(conn->rx, &header);
		if (!mbap_is_valid(&header))
		{
			LOG_WARN("client %s sent a bad MBAP header, closing", conn->base.peer);
			net_obs_rejected(server->options.obs);
			conn_close(server, conn, NET_CLOSE_ERROR);
			return;
		}

		frame = mbap_frame_size(&header);
		if (conn->rx_len < frame)
			return;

		handle_frame(server, conn, frame, os_now_ms());
		if (!os_socket_valid(conn->base.fd))
			return; /* a fault closed the connection */
		conn_consume(conn, frame);
	}
}

static void
conn_tx_advance(tcp_server_t* server, tcp_conn_t* conn, u32 sent)
{
	conn->tx_sent         += sent;
	conn->base.last_activity_ms = os_now_ms();
	if (conn->tx_sent < conn->tx_len)
		return;

	conn->tx_len  = 0;
	conn->tx_sent = 0;
	conn_process(server, conn);	/* a pipelined request may already be waiting */
}

static void
conn_read(tcp_server_t* server, tcp_conn_t* conn)
{
	s64	got;

	if (conn->tls)
	{
		int			len      = 0;
		net_tls_status_t	status;

		if (conn->rx_len == sizeof(conn->rx))
		{
			LOG_WARN("client %s receive buffer full, closing", conn->base.peer);
			conn_close(server, conn, NET_CLOSE_ERROR);
			return;
		}

		status = net_tls_conn_read(conn->tls, conn->rx + conn->rx_len,
					   (int)(sizeof(conn->rx) - conn->rx_len), &len);
		if (status == NET_TLS_OK && len > 0)
		{
			conn->rx_len          += (u32)len;
			conn->base.last_activity_ms = os_now_ms();
			conn_process(server, conn);
			return;
		}
		if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
			return;
		if (status == NET_TLS_ERROR && len == 0)
			LOG_WARN("client %s TLS read failed: %s", conn->base.peer, conn->tls->error);
		conn_close(server, conn, status == NET_TLS_ERROR ? NET_CLOSE_ERROR : NET_CLOSE_PEER);
		return;
	}

	got = os_socket_recv(conn->base.fd, conn->rx + conn->rx_len, sizeof(conn->rx) - conn->rx_len);
	if (got == 0 || (got < 0 && !os_retry()))
	{
		conn_close(server, conn, got == 0 ? NET_CLOSE_PEER : NET_CLOSE_ERROR);
		return;
	}
	if (got < 0)
		return;

	conn->rx_len          += (u32)got;
	conn->base.last_activity_ms = os_now_ms();
	conn_process(server, conn);
}

static void
conn_write(tcp_server_t* server, tcp_conn_t* conn)
{
	s64	sent;

	if (conn->tls)
	{
		int			len      = 0;
		net_tls_status_t	status;

		status = net_tls_conn_write(conn->tls, conn->tx + conn->tx_sent,
					    (int)(conn->tx_len - conn->tx_sent), &len);
		if (status == NET_TLS_OK && len > 0)
		{
			conn_tx_advance(server, conn, (u32)len);
			return;
		}
		if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
			return;
		if (status == NET_TLS_ERROR && len == 0)
			LOG_WARN("client %s TLS write failed: %s", conn->base.peer, conn->tls->error);
		conn_close(server, conn, NET_CLOSE_ERROR);
		return;
	}

	sent = os_socket_send(conn->base.fd, conn->tx + conn->tx_sent, conn->tx_len - conn->tx_sent);
	if (sent < 0 &&
	    !os_retry())
	{
		conn_close(server, conn, NET_CLOSE_ERROR);
		return;
	}
	if (sent < 0)
		return;

	conn_tx_advance(server, conn, (u32)sent);
}

/* --- main loop --- */

/** @brief A TLS handshake has its own deadline from accept(), even with idle timeout disabled. */
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

		if (!os_socket_valid(conn->base.fd) ||
		    !conn->tls ||
		    net_tls_conn_handshake_done(conn->tls) ||
		    deadline == 0 ||
		    now_ms < deadline)
			continue;

		LOG_WARN("client %s TLS handshake exceeded %u ms, closing", conn->base.peer, timeout_ms);
		conn_close(server, conn, NET_CLOSE_TLS);
	}
}

/**
 * @brief OpenSSL may hold decrypted request bytes even when the socket has nothing to read.
 *
 * Only synthesize a wakeup while no response is pending, so delayed faults are honored.
 */
static bool
conn_tls_has_buffered_input(const tcp_conn_t* conn)
{
	return conn->tls &&
	       net_tls_conn_handshake_done(conn->tls) &&
	       conn->tx_len == 0 &&
	       net_tls_conn_pending(conn->tls) > 0;
}

/**
 * @brief Fills the poll table.
 *
 * Returns the number of entries; entry 0 is the listening socket.
 */
static u32
build_poll_table(tcp_server_t* server, os_wait_entry_t* fds, net_tcp_conn_t** owners,
		 u64 now_ms, int* timeout)
{
	u32 count = net_tcp_pool_build_poll(&server->pool, fds, owners, now_ms, timeout);

	for (u32 i = 1; i < count; i++)
	{
		tcp_conn_t*	conn         = (tcp_conn_t*)owners[i];
		u32	tls_events   = conn->tls ? net_tls_conn_poll_events(conn->tls) : 0;
		u64	handshake_deadline;
		u32	events = OS_WAIT_IN;

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
		else if (conn->tls && conn->tx_len == 0 && (tls_events & OS_WAIT_OUT))
			events = OS_WAIT_OUT;

		/* Decrypted bytes may already be buffered inside OpenSSL. */
		if (conn_tls_has_buffered_input(conn))
		{
			events |= OS_WAIT_IN;
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
				events = OS_WAIT_OUT;
		}
		else
			events = (conn->tx_due_ms <= now_ms) ? OS_WAIT_OUT : 0;
		fds[i].events = events;
	}
	return count;
}

static void
serve_ready(tcp_server_t* server, const os_wait_entry_t* fd, net_tcp_conn_t* owner, u64 now_ms)
{
	tcp_conn_t*	conn          = (tcp_conn_t*)owner;
	bool	buffered_input = owner && conn_tls_has_buffered_input(conn);
	u32	tls_events;

	if (fd->revents == 0 && !buffered_input)
		return;

	if (!owner)
	{
		net_tcp_pool_accept(&server->pool);
		return;
	}

	if (!os_socket_valid(conn->base.fd) || fd->fd != conn->base.fd)
		return;

	if (fd->revents & (OS_WAIT_ERR | OS_WAIT_INVAL))
	{
		conn_close(server, conn, NET_CLOSE_ERROR);
		return;
	}

	if (conn->tls && !net_tls_conn_handshake_done(conn->tls))
	{
		if (!conn_tls_handshake(server, conn) || !net_tls_conn_handshake_done(conn->tls))
			return;
		if ((fd->revents & (OS_WAIT_IN | OS_WAIT_HUP)) != 0 ||
		    conn_tls_has_buffered_input(conn))
			conn_read(server, conn);
		return;
	}

	tls_events = conn->tls ? net_tls_conn_poll_events(conn->tls) : 0;

	/** WANT_WRITE from SSL_read must be retried as a read, never as an empty write. */
	if (conn->tls && conn->tx_len == 0 && tls_events != 0)
	{
		conn_read(server, conn);
		return;
	}

	/**
	 * @brief A TLS write can need a read (renegotiation/post-handshake messages), but a delayed
	 *        response must never be sent early.
	 *
	 * Do not consume new request bytes while a response is still pending.
	 */
	if (conn->tls && conn->tx_len > 0)
	{
		/* poll reports OS_WAIT_HUP even when events is 0: a disconnect must free
		 * the slot without waiting for the delayed response to become due. */
		if (fd->revents & OS_WAIT_HUP)
		{
			conn_close(server, conn, NET_CLOSE_PEER);
			return;
		}
		if (tls_events != 0 || conn->tx_due_ms <= now_ms)
			conn_write(server, conn);
		return;
	}

	if (fd->revents & OS_WAIT_OUT)
	{
		conn_write(server, conn);
		return;
	}
	if ((fd->revents & (OS_WAIT_IN | OS_WAIT_HUP)) != 0 || buffered_input)
		conn_read(server, conn);
}

s32
tcp_server_step(tcp_server_t* server, s32 max_wait_ms)
{
	os_wait_entry_t		fds[TCP_SERVER_MAX_CONNECTIONS + 1];
	net_tcp_conn_t*		owners[TCP_SERVER_MAX_CONNECTIONS + 1];
	u32		count   = 0;
	int		timeout = 0;
	u64		now_ms  = os_now_ms();

	if (net_stop_requested(server->options.obs, server->stop))
		return NET_STEP_DONE;

	net_tcp_pool_close_idle(&server->pool, now_ms);
	close_stalled_handshakes(server, now_ms);
	count = build_poll_table(server, fds, owners, now_ms, &timeout);
	if (max_wait_ms >= 0)
		timeout = MIN(timeout, max_wait_ms);
	if (net_poll(server->options.obs, fds, count, timeout) < 0)
		return NET_STEP_CONTINUE;	/* EINTR: the caller re-checks stop */

	now_ms = os_now_ms();
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
	memset(server, 0, sizeof(*server));
	server->options = *options;
	server->pool.conns = (net_tcp_conn_t*)server->conns;
	server->pool.conn_count = ARRAY_COUNT(server->conns);
	server->pool.conn_size = sizeof(server->conns[0]);
	server->pool.idle_timeout_ms = options->idle_timeout_ms;
	server->pool.listen_fd = OS_SOCKET_INVALID;
	server->pool.ctx = server;
	server->pool.obs = options->obs;
	server->pool.on_accept = conn_on_accept;
	server->pool.on_close = conn_on_close;
	server->pool.skip_idle = conn_skip_idle;
	rng_seed(&server->rng, options->seed);
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
		server->conns[i].base.fd = OS_SOCKET_INVALID;

	if (options->tls_enabled)
	{
		if (!net_tls_ctx_init(&server->tls, &options->tls))
		{
			LOG_WARN("cannot configure TLS: %s", server->tls.error);
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
	bool was_listening = os_socket_valid(server->pool.listen_fd);

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
