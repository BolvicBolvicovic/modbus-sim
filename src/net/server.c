#define _GNU_SOURCE
#include "net/server.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_MAX_CONNECTIONS	32
#define SERVER_IDLE_POLL_MS	1000

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
	char	peer[INET_ADDRSTRLEN + 8];
};

typedef struct server_t server_t;
struct server_t
{
	server_options_t	options;
	int			listen_fd;
	rng_t			rng;
	conn_t			conns[SERVER_MAX_CONNECTIONS];
};

static volatile sig_atomic_t	g_stop = 0;

static void
on_stop_signal(int signo)
{
	(void)signo;
	g_stop = 1;
}

static void
install_signal_handlers(void)
{
	struct sigaction	sa = { 0 };

	sa.sa_handler = on_stop_signal;	/* no SA_RESTART: poll() must wake up */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* --- listening socket --- */

static s32
listen_open(server_t* server)
{
	struct sockaddr_in	addr = { 0 };
	int			yes  = 1;
	int			fd   = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(server->options.port);
	if (inet_pton(AF_INET, server->options.bind_address, &addr.sin_addr) != 1)
	{
		LOG_WARN("bind address '%s' is not an IPv4 address", server->options.bind_address);
		close(fd);
		return -1;
	}

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0)
	{
		LOG_WARN("cannot listen on %s:%u: %s", server->options.bind_address,
			 server->options.port, strerror(errno));
		close(fd);
		return -1;
	}

	server->listen_fd = fd;
	return 0;
}

/* --- connections --- */

static void
conn_close(conn_t* conn)
{
	LOG_DEBUG("client %s disconnected", conn->peer);
	close(conn->fd);
	conn->fd     = -1;
	conn->rx_len = 0;
	conn->tx_len = 0;
}

static conn_t*
conn_find_free(server_t* server)
{
	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		if (server->conns[i].fd < 0)
			return &server->conns[i];
	}
	return NULL;
}

static void
accept_clients(server_t* server)
{
	struct sockaddr_in	addr = { 0 };
	socklen_t		len  = sizeof(addr);
	char			ip[INET_ADDRSTRLEN];
	int			fd   = 0;
	conn_t*			conn = NULL;

	for (;;)
	{
		len = sizeof(addr);
		fd  = accept4(server->listen_fd, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK);
		if (fd < 0)
			return;

		conn = conn_find_free(server);
		if (!conn)
		{
			LOG_WARN("too many clients, refusing a new one");
			close(fd);
			continue;
		}

		inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		memset(conn, 0, sizeof(*conn));
		conn->fd               = fd;
		conn->last_activity_ms = clock_now_ms();
		snprintf(conn->peer, sizeof(conn->peer), "%s:%u", ip, ntohs(addr.sin_port));
		LOG_DEBUG("client %s connected", conn->peer);
	}
}

static device_t*
find_device(const server_t* server, u8 unit)
{
	for (u32 i = 0; i < server->options.device_count; i++)
	{
		if (server->options.devices[i]->unit_id == unit)
			return server->options.devices[i];
	}
	return NULL;
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
handle_frame(server_t* server, conn_t* conn, const mbap_header_t* header, u64 now_ms)
{
	u8			pdu[MODBUS_MAX_PDU];
	device_t*		dev      = find_device(server, header->unit);
	const u8*		request  = conn->rx + MBAP_HEADER_SIZE;
	u32			req_len  = (u32)header->length - 1;
	u32			pdu_len  = 0;
	fault_decision_t	decision = { 0 };
	mbap_header_t		reply    = *header;

	if (!dev)
		pdu_len = modbus_exception(pdu, request[0], EXC_GATEWAY_TARGET_FAILED);
	else
		pdu_len = modbus_handle_pdu(dev, now_ms, request, req_len, pdu);

	if (dev)
		decision = fault_decide(&dev->fault, &server->rng);

	LOG_DEBUG("%s tid=%u unit=%u fc=0x%02X -> %s%s", conn->peer, header->transaction,
		  header->unit, request[0], (pdu[0] & 0x80) ? "exception" : "ok",
		  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");

	if (decision.drop)
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
conn_process(server_t* server, conn_t* conn)
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
conn_read(server_t* server, conn_t* conn)
{
	ssize_t	got = recv(conn->fd, conn->rx + conn->rx_len, sizeof(conn->rx) - conn->rx_len, 0);

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
conn_write(server_t* server, conn_t* conn)
{
	ssize_t	sent = send(conn->fd, conn->tx + conn->tx_sent, conn->tx_len - conn->tx_sent,
			    MSG_NOSIGNAL);

	if (sent < 0 && errno != EAGAIN && errno != EINTR)
	{
		conn_close(conn);
		return;
	}

	if (sent < 0)
		return;

	conn->tx_sent         += (u32)sent;
	conn->last_activity_ms = clock_now_ms();
	if (conn->tx_sent < conn->tx_len)
		return;

	conn->tx_len  = 0;
	conn->tx_sent = 0;
	conn_process(server, conn);	/* a pipelined request may already be waiting */
}

/* --- main loop --- */

static void
close_idle_connections(server_t* server, u64 now_ms)
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

/* Fills the poll table. Returns the number of entries; entry 0 is the listening socket. */
static u32
build_poll_table(server_t* server, struct pollfd* fds, conn_t** owners, u64 now_ms, int* timeout)
{
	u32	count   = 0;
	u32	idle_ms = server->options.idle_timeout_ms;

	*timeout = SERVER_IDLE_POLL_MS;
	fds[count] = (struct pollfd){ .fd = server->listen_fd, .events = POLLIN };
	owners[count++] = NULL;

	for (u32 i = 0; i < ARRAY_COUNT(server->conns); i++)
	{
		conn_t*	conn = &server->conns[i];
		u64	idle_deadline;

		if (conn->fd < 0)
			continue;

		fds[count] = (struct pollfd){ .fd = conn->fd, .events = POLLIN };
		owners[count++] = conn;
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
			continue;
		}

		/* A response is pending: wait for its due time, then for the socket to be writable. */
		fds[count - 1].events = (conn->tx_due_ms <= now_ms) ? POLLOUT : 0;
		if (conn->tx_due_ms > now_ms)
			*timeout = MIN(*timeout, (int)(conn->tx_due_ms - now_ms));
	}
	return count;
}

static void
serve_ready(server_t* server, const struct pollfd* fd, conn_t* conn)
{
	if (fd->revents == 0)
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

	if (fd->revents & POLLOUT)
		conn_write(server, conn);
	else if (fd->revents & (POLLIN | POLLHUP))
		conn_read(server, conn);
}

static void
serve_loop(server_t* server)
{
	struct pollfd	fds[SERVER_MAX_CONNECTIONS + 1];
	conn_t*		owners[SERVER_MAX_CONNECTIONS + 1];
	u32		count   = 0;
	int		timeout = 0;
	u64		now_ms  = 0;

	while (!g_stop)
	{
		now_ms = clock_now_ms();
		close_idle_connections(server, now_ms);
		count = build_poll_table(server, fds, owners, now_ms, &timeout);
		if (poll(fds, count, timeout) < 0)
			continue;

		now_ms = clock_now_ms();
		close_idle_connections(server, now_ms);
		for (u32 i = 0; i < count; i++)
			serve_ready(server, &fds[i], owners[i]);
	}
}

s32
server_run(const server_options_t* options)
{
	server_t server;

	server.options = *options;
	rng_seed(&server.rng, options->seed);
	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
		server.conns[i].fd = -1;

	install_signal_handlers();
	if (listen_open(&server) < 0)
		return 2;

	LOG_INFO("listening on %s:%u (%u device%s). Ctrl-C to stop.", options->bind_address,
		 options->port, options->device_count, options->device_count == 1 ? "" : "s");
	serve_loop(&server);

	for (u32 i = 0; i < ARRAY_COUNT(server.conns); i++)
	{
		if (server.conns[i].fd >= 0)
			close(server.conns[i].fd);
	}
	close(server.listen_fd);
	LOG_INFO("stopped");
	return 0;
}
