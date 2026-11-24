#define _GNU_SOURCE
#include "net/tcp_server.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TCP_SERVER_MAX_CONNECTIONS	32
#define TCP_SERVER_IDLE_POLL_MS	1000
#define TCP_SERVER_ACCEPT_BACKOFF_MS 1000

/* Name: conn_t
 * Description: one client connection. A connection handles one request at a time:
 * while a response is waiting to be sent (tx_len > 0) no new request is read.
 * */
typedef struct conn_t conn_t;
struct conn_t
{
	int	fd;			/* -1 when the slot is free */
	u8	rx[MBAP_MAX_FRAME];
	u32	rx_len;
	u8	tx[MBAP_MAX_FRAME];
	u32	tx_len;
	u32	tx_sent;
	u64	tx_due_ms;		/* do not send before this time (delay fault) */
	u64	last_activity_ms;	/* updated on I/O progress; used for idle timeout */
	u64	tls_handshake_deadline_ms; /* measured from accept; 0 = no deadline */
	char	peer[INET6_ADDRSTRLEN + 8];
	SSL*	ssl;			/* NULL for plaintext connections */
	bool	tls_handshake_done;
	bool	tls_want_read;
	bool	tls_want_write;
};

typedef struct tcp_server_t tcp_server_t;
struct tcp_server_t
{
	tcp_server_options_t	options;
	net_tls_ctx_t*		tls;
	int			listen_fd;
	u64			accept_backoff_until_ms;
	bool			accept_backoff_logged;
	rng_t			rng;
	conn_t			conns[TCP_SERVER_MAX_CONNECTIONS];
};

static volatile sig_atomic_t	g_stop = 0;

/* --- listening socket --- */

static s32
listen_open(tcp_server_t* server)
{
	int fd = net_tcp_listen(server->options.bind_address, server->options.port, 64);
	if (fd < 0)
		return -1;
	server->listen_fd = fd;
	return 0;
}

/* --- connections --- */

static void
conn_close(conn_t* conn)
{
	LOG_DEBUG("client %s disconnected", conn->peer);
	if (conn->ssl)
	{
		net_tls_session_close(conn->ssl);
		conn->ssl = NULL;
	}
	close(conn->fd);
	conn->fd     = -1;
	conn->rx_len = 0;
	conn->tx_len = 0;
}

static bool
conn_tls_handshake(conn_t* conn)
{
	char			tls_err[256];
	char			subject[512];
	net_tls_status_t	status;

	conn->tls_want_read  = false;
	conn->tls_want_write = false;
	status = net_tls_handshake(conn->ssl, tls_err, sizeof(tls_err));
	if (status == NET_TLS_OK)
	{
		conn->tls_handshake_done = true;
		conn->last_activity_ms   = clock_now_ms();
		LOG_DEBUG("TLS handshake complete with %s", conn->peer);
		if (net_tls_peer_subject(conn->ssl, subject, sizeof(subject)))
			LOG_INFO("client %s authenticated TLS peer subject: %s", conn->peer, subject);
		return true;
	}
	if (status == NET_TLS_WANT_READ)
	{
		conn->tls_want_read = true;
		return true;
	}
	if (status == NET_TLS_WANT_WRITE)
	{
		conn->tls_want_write = true;
		return true;
	}
	if (status == NET_TLS_ERROR)
		LOG_WARN("client %s TLS handshake failed: %s", conn->peer, tls_err);
	conn_close(conn);
	return false;
}

static conn_t*
conn_find_free(tcp_server_t* server)
{
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		if (server->conns[i].fd < 0)
			return &server->conns[i];
	}
	return NULL;
}

static void
accept_clients(tcp_server_t* server)
{
	struct sockaddr_storage	addr = { 0 };
	socklen_t		len  = sizeof(addr);
	char			ip[INET6_ADDRSTRLEN];
	char			tls_err[256];
	u64			now_ms = 0;
	int			fd   = 0;
	int			yes  = 1;
	conn_t*			conn = NULL;

	for (;;)
	{
		len = sizeof(addr);
		fd  = accept4(server->listen_fd, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK);
		if (fd < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (errno == EMFILE || errno == ENFILE)
			{
				if (!server->accept_backoff_logged)
				{
					LOG_WARN("accept failed: %s; backing off for %u ms",
						 strerror(errno), TCP_SERVER_ACCEPT_BACKOFF_MS);
					server->accept_backoff_logged = true;
				}
				server->accept_backoff_until_ms = clock_now_ms() + TCP_SERVER_ACCEPT_BACKOFF_MS;
				return;
			}

			LOG_WARN("accept failed: %s", strerror(errno));
			return;
		}

		server->accept_backoff_logged = false;

		conn = conn_find_free(server);
		if (!conn)
		{
			LOG_WARN("too many clients, refusing a new one");
			close(fd);
			continue;
		}

		if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
			LOG_WARN("cannot set TCP_NODELAY on accepted socket: %s", strerror(errno));

		now_ms                 = clock_now_ms();
		memset(conn, 0, sizeof(*conn));
		conn->fd               = fd;
		conn->last_activity_ms = now_ms;
		if (server->options.tls_handshake_timeout_ms > 0)
			conn->tls_handshake_deadline_ms =
				now_ms + server->options.tls_handshake_timeout_ms;
		if (addr.ss_family == AF_INET6)
		{
			struct sockaddr_in6* a = (struct sockaddr_in6*)&addr;

			inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
			snprintf(conn->peer, sizeof(conn->peer), "[%s]:%u", ip, ntohs(a->sin6_port));
		}
		else
		{
			struct sockaddr_in* a = (struct sockaddr_in*)&addr;

			inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
			snprintf(conn->peer, sizeof(conn->peer), "%s:%u", ip, ntohs(a->sin_port));
		}
		LOG_DEBUG("client %s connected", conn->peer);
		if (server->tls)
		{
			conn->ssl = net_tls_session_new(server->tls, fd, tls_err, sizeof(tls_err));
			if (!conn->ssl)
			{
				LOG_WARN("client %s TLS setup failed: %s", conn->peer, tls_err);
				conn_close(conn);
				continue;
			}
			conn->tls_want_read = true;
			(void)conn_tls_handshake(conn);
		}
	}
}



/* Removes the first `size` bytes of the receive buffer. */
static void
conn_consume(conn_t* conn, u32 size)
{
	memmove(conn->rx, conn->rx + size, conn->rx_len - size);
	conn->rx_len -= size;
}

/* Runs the request found at the start of conn->rx and queues the response (if any). */
static void
handle_frame(tcp_server_t* server, conn_t* conn, const mbap_header_t* header, u64 now_ms)
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

	LOG_DEBUG("%s tid=%u unit=%u fc=0x%02X -> %s%s", conn->peer, header->transaction,
		  header->unit, request[0], pdu_len == 0 ? "no response" :
		  (pdu[0] & 0x80) ? "exception" : "ok",
		  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");

	if (decision.drop || pdu_len == 0)
	{
		conn->last_activity_ms = now_ms;
		return;
	}

	reply.length = (u16)(pdu_len + 1);
	mbap_encode(conn->tx, &reply);
	memcpy(conn->tx + MBAP_HEADER_SIZE, pdu, pdu_len);
	conn->tx_len           = MBAP_HEADER_SIZE + pdu_len;
	conn->tx_sent          = 0;
	conn->tx_due_ms        = now_ms + decision.delay_ms;
	conn->last_activity_ms = now_ms;
}

/* Handles every complete request waiting in the receive buffer, one at a time. */
static void
conn_process(tcp_server_t* server, conn_t* conn)
{
	mbap_header_t	header = { 0 };
	u32		frame  = 0;

	while (conn->fd >= 0 && conn->tx_len == 0 && conn->rx_len >= MBAP_HEADER_SIZE)
	{
		mbap_decode(conn->rx, &header);
		if (!mbap_is_valid(&header))
		{
			LOG_WARN("client %s sent a bad MBAP header, closing", conn->peer);
			conn_close(conn);
			return;
		}

		frame = mbap_frame_size(&header);
		if (conn->rx_len < frame)
			return;

		handle_frame(server, conn, &header, clock_now_ms());
		conn_consume(conn, frame);
	}
}

static void
conn_tx_advance(tcp_server_t* server, conn_t* conn, u32 sent)
{
	conn->tx_sent         += sent;
	conn->last_activity_ms = clock_now_ms();
	if (conn->tx_sent < conn->tx_len)
		return;

	conn->tx_len  = 0;
	conn->tx_sent = 0;
	conn_process(server, conn);	/* a pipelined request may already be waiting */
}

static void
conn_read(tcp_server_t* server, conn_t* conn)
{
	ssize_t	got;

	if (conn->ssl)
	{
		char			tls_err[256];
		int			len      = 0;
		net_tls_status_t	status;

		if (conn->rx_len == sizeof(conn->rx))
		{
			LOG_WARN("client %s receive buffer full, closing", conn->peer);
			conn_close(conn);
			return;
		}

		conn->tls_want_read  = false;
		conn->tls_want_write = false;
		status = net_tls_read(conn->ssl, conn->rx + conn->rx_len,
				      (int)(sizeof(conn->rx) - conn->rx_len), &len,
				      tls_err, sizeof(tls_err));
		if (status == NET_TLS_OK && len > 0)
		{
			conn->rx_len          += (u32)len;
			conn->last_activity_ms = clock_now_ms();
			conn_process(server, conn);
			return;
		}
		if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
		{
			conn->tls_want_read  = status == NET_TLS_WANT_READ;
			conn->tls_want_write = status == NET_TLS_WANT_WRITE;
			return;
		}
		if (status == NET_TLS_ERROR && len == 0)
			LOG_WARN("client %s TLS read failed: %s", conn->peer, tls_err);
		conn_close(conn);
		return;
	}

	got = recv(conn->fd, conn->rx + conn->rx_len, sizeof(conn->rx) - conn->rx_len, 0);
	if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR))
	{
		conn_close(conn);
		return;
	}
	if (got < 0)
		return;

	conn->rx_len          += (u32)got;
	conn->last_activity_ms = clock_now_ms();
	conn_process(server, conn);
}

static void
conn_write(tcp_server_t* server, conn_t* conn)
{
	ssize_t	sent;

	if (conn->ssl)
	{
		char			tls_err[256];
		int			len      = 0;
		net_tls_status_t	status;

		conn->tls_want_read  = false;
		conn->tls_want_write = false;
		status = net_tls_write(conn->ssl, conn->tx + conn->tx_sent,
				       (int)(conn->tx_len - conn->tx_sent), &len,
				       tls_err, sizeof(tls_err));
		if (status == NET_TLS_OK && len > 0)
		{
			conn_tx_advance(server, conn, (u32)len);
			return;
		}
		if (status == NET_TLS_WANT_READ || status == NET_TLS_WANT_WRITE)
		{
			conn->tls_want_read  = status == NET_TLS_WANT_READ;
			conn->tls_want_write = status == NET_TLS_WANT_WRITE;
			return;
		}
		if (status == NET_TLS_ERROR && len == 0)
			LOG_WARN("client %s TLS write failed: %s", conn->peer, tls_err);
		conn_close(conn);
		return;
	}

	sent = send(conn->fd, conn->tx + conn->tx_sent, conn->tx_len - conn->tx_sent,
		    MSG_NOSIGNAL);
	if (sent < 0 &&
	    errno != EAGAIN &&
	    errno != EINTR)
	{
		conn_close(conn);
		return;
	}
	if (sent < 0)
		return;

	conn_tx_advance(server, conn, (u32)sent);
}

/* --- main loop --- */

static void
close_idle_connections(tcp_server_t* server, u64 now_ms)
{
	u32	idle_ms = server->options.idle_timeout_ms;

	if (idle_ms == 0)
		return;

	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		conn_t*	conn = &server->conns[i];
		u64	deadline;

		if (conn->fd < 0 || conn->tx_len > 0)
			continue;

		deadline = conn->last_activity_ms + idle_ms;
		if (now_ms < deadline)
			continue;

		LOG_INFO("client %s idle for %u ms, closing", conn->peer, idle_ms);
		conn_close(conn);
	}
}

/* A TLS handshake has its own deadline from accept(), even with idle timeout disabled. */
static void
close_stalled_handshakes(tcp_server_t* server, u64 now_ms)
{
	u32 timeout_ms = server->options.tls_handshake_timeout_ms;

	if (!server->tls || timeout_ms == 0)
		return;

	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		conn_t* conn = &server->conns[i];

		if (conn->fd < 0 ||
		    !conn->ssl ||
		    conn->tls_handshake_done ||
		    conn->tls_handshake_deadline_ms == 0 ||
		    now_ms < conn->tls_handshake_deadline_ms)
			continue;

		LOG_WARN("client %s TLS handshake exceeded %u ms, closing", conn->peer, timeout_ms);
		conn_close(conn);
	}
}

/* OpenSSL may hold decrypted request bytes even when the socket has nothing to read.
 * Only synthesize a wakeup while no response is pending, so delayed faults are honored. */
static bool
conn_tls_has_buffered_input(const conn_t* conn)
{
	return conn->ssl &&
	       conn->tls_handshake_done &&
	       conn->tx_len == 0 &&
	       net_tls_pending(conn->ssl) > 0;
}

/* Fills the poll table. Returns the number of entries; entry 0 is the listening socket. */
static u32
build_poll_table(tcp_server_t* server, struct pollfd* fds, conn_t** owners, u64 now_ms, int* timeout)
{
	u32	count   = 0;
	u32	idle_ms = server->options.idle_timeout_ms;
	bool	backoff = server->accept_backoff_until_ms > now_ms;

	*timeout = TCP_SERVER_IDLE_POLL_MS;
	fds[count] = (struct pollfd){ .fd = server->listen_fd, .events = backoff ? 0 : POLLIN };
	owners[count++] = NULL;
	if (backoff)
		*timeout = MIN(*timeout, (int)(server->accept_backoff_until_ms - now_ms));

	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		conn_t*	conn   = &server->conns[i];
		u64	idle_deadline;
		u64	handshake_deadline;
		short	events = POLLIN;

		if (conn->fd < 0)
			continue;

		fds[count] = (struct pollfd){ .fd = conn->fd, .events = events };
		owners[count++] = conn;

		if (conn->ssl && !conn->tls_handshake_done)
		{
			events = 0;
			if (conn->tls_want_read)
				events |= POLLIN;
			if (conn->tls_want_write)
				events |= POLLOUT;
			if (conn->tls_handshake_deadline_ms > 0)
			{
				handshake_deadline = conn->tls_handshake_deadline_ms;
				if (handshake_deadline <= now_ms)
					*timeout = 0;
				else
					*timeout = MIN(*timeout, (int)(handshake_deadline - now_ms));
			}
		}
		else if (conn->ssl && conn->tx_len == 0 && conn->tls_want_write)
			events = POLLOUT;

		/* Decrypted bytes may already be buffered inside OpenSSL. */
		if (conn_tls_has_buffered_input(conn))
		{
			events |= POLLIN;
			*timeout = 0;
		}

		if (conn->tx_len == 0)
		{
			if (idle_ms > 0)
			{
				idle_deadline = conn->last_activity_ms + idle_ms;
				if (idle_deadline <= now_ms)
					*timeout = 0;
				else
					*timeout = MIN(*timeout, (int)(idle_deadline - now_ms));
			}
			fds[count - 1].events = events;
			continue;
		}

		/* A response is pending: wait for its due time, then for the socket to be writable. */
		if (conn->tx_due_ms > now_ms)
			*timeout = MIN(*timeout, (int)(conn->tx_due_ms - now_ms));
		if (conn->ssl)
		{
			events = 0;
			if (conn->tls_want_read)
				events |= POLLIN;
			if (conn->tls_want_write)
				events |= POLLOUT;
			if (!conn->tls_want_read && !conn->tls_want_write &&
			    conn->tx_due_ms <= now_ms)
				events = POLLOUT;
		}
		else
			events = (conn->tx_due_ms <= now_ms) ? POLLOUT : 0;
		fds[count - 1].events = events;
	}
	return count;
}

static void
serve_ready(tcp_server_t* server, const struct pollfd* fd, conn_t* conn, u64 now_ms)
{
	bool buffered_input = conn && conn_tls_has_buffered_input(conn);

	if (fd->revents == 0 && !buffered_input)
		return;

	if (!conn)
	{
		accept_clients(server);
		return;
	}

	if (conn->fd < 0 || fd->fd != conn->fd)
		return;

	if (fd->revents & (POLLERR | POLLNVAL))
	{
		conn_close(conn);
		return;
	}

	if (conn->ssl && !conn->tls_handshake_done)
	{
		if (!conn_tls_handshake(conn) || !conn->tls_handshake_done)
			return;
		if ((fd->revents & (POLLIN | POLLHUP)) != 0 ||
		    conn_tls_has_buffered_input(conn))
			conn_read(server, conn);
		return;
	}

	/* WANT_WRITE from SSL_read must be retried as a read, never as an empty write. */
	if (conn->ssl &&
	    conn->tx_len == 0 &&
	    (conn->tls_want_read || conn->tls_want_write))
	{
		conn_read(server, conn);
		return;
	}

	/* A TLS write can need a read (renegotiation/post-handshake messages), but
	 * a delayed response must never be sent early. Do not consume new request
	 * bytes while a response is still pending. */
	if (conn->ssl && conn->tx_len > 0)
	{
		/* poll reports POLLHUP even when events is 0: a disconnect must free
		 * the slot without waiting for the delayed response to become due. */
		if (fd->revents & POLLHUP)
		{
			conn_close(conn);
			return;
		}
		if (conn->tls_want_read ||
		    conn->tls_want_write ||
		    conn->tx_due_ms <= now_ms)
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

static void
serve_loop(tcp_server_t* server)
{
	struct pollfd	fds[TCP_SERVER_MAX_CONNECTIONS + 1];
	conn_t*		owners[TCP_SERVER_MAX_CONNECTIONS + 1];
	u32		count   = 0;
	int		timeout = 0;
	u64		now_ms  = 0;

	while (!g_stop)
	{
		now_ms = clock_now_ms();
		close_idle_connections(server, now_ms);
		close_stalled_handshakes(server, now_ms);
		count = build_poll_table(server, fds, owners, now_ms, &timeout);
		if (poll(fds, count, timeout) < 0)
			continue;

		now_ms = clock_now_ms();
		close_idle_connections(server, now_ms);
		close_stalled_handshakes(server, now_ms);
		for (u32 i = 0; i < count; i++)
			serve_ready(server, &fds[i], owners[i], now_ms);
	}
}

s32
tcp_server_run(const tcp_server_options_t* options)
{
	tcp_server_t		server = { 0 };
	char			tls_err[256];

	server.options = *options;
	rng_seed(&server.rng, options->seed);
	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
		server.conns[i].fd = -1;

	if (options->tls_enabled)
	{
		server.tls = net_tls_ctx_create(&options->tls, tls_err, sizeof(tls_err));
		if (!server.tls)
		{
			LOG_WARN("cannot configure TLS: %s", tls_err);
			return 2;
		}
	}

	g_stop = 0;
	net_install_stop_handlers(&g_stop);
	if (listen_open(&server) < 0)
	{
		net_tls_ctx_destroy(server.tls);
		return 2;
	}

	LOG_INFO("listening on %s:%u (%u device%s). Ctrl-C to stop.", options->bind_address,
		 options->port, options->device_count, options->device_count == 1 ? "" : "s");
	if (server.tls)
		LOG_INFO("TLS enabled: certificate=%s private-key=%s CA=%s client-certificate=%s",
			 options->tls.certificate, options->tls.private_key,
			 options->tls.ca_certificate ? options->tls.ca_certificate : "(none)",
			 options->tls.require_client_certificate ? "required" : "optional");
	serve_loop(&server);

	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
	{
		if (server.conns[i].fd >= 0)
		{
			if (server.conns[i].ssl)
				net_tls_session_close(server.conns[i].ssl);
			close(server.conns[i].fd);
		}
	}
	close(server.listen_fd);
	net_tls_ctx_destroy(server.tls);
	LOG_INFO("stopped");
	return 0;
}
