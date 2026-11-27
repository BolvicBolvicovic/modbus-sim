#include "net/net_tls.h"
#include <stdio.h>
#include <string.h>

/**
 * @file
 * @brief No-TLS implementation of net_tls.h, built when WITH_TLS=0.
 *
 * The API stays linkable for the shared net/tcp_server code, but no TLS session can ever be
 * created. Config loading rejects (tls ...) before this is reached; the error text is kept here too so
 * direct callers get the same explanation.
 */

static void
tls_unavailable(char* error, u32 size)
{
	snprintf(error, size, "%s", "built without TLS support");
}

bool
net_tls_ctx_init(net_tls_ctx_t* ctx, const net_tls_config_t* config)
{
	(void)config;
	memset(ctx, 0, sizeof(*ctx));
	tls_unavailable(ctx->error, sizeof(ctx->error));
	return false;
}

void
net_tls_ctx_close(net_tls_ctx_t* ctx)
{
	(void)ctx;
}

net_tls_conn_t*
net_tls_conn_new(net_tls_ctx_t* ctx, int fd, u64 handshake_deadline_ms)
{
	(void)fd;
	(void)handshake_deadline_ms;
	tls_unavailable(ctx->error, sizeof(ctx->error));
	return NULL;
}

void
net_tls_conn_close(net_tls_conn_t* conn)
{
	(void)conn;
}

bool
net_tls_conn_handshake_done(const net_tls_conn_t* conn)
{
	(void)conn;
	return false;
}

u64
net_tls_conn_handshake_deadline(const net_tls_conn_t* conn)
{
	(void)conn;
	return 0;
}

u32
net_tls_conn_poll_events(const net_tls_conn_t* conn)
{
	(void)conn;
	return 0;
}

int
net_tls_conn_pending(const net_tls_conn_t* conn)
{
	(void)conn;
	return 0;
}

bool
net_tls_conn_peer_subject(const net_tls_conn_t* conn, char* buf, u32 size)
{
	(void)conn;
	if (buf && size)
		buf[0] = '\0';
	return false;
}

net_tls_status_t
net_tls_conn_handshake(net_tls_conn_t* conn)
{
	tls_unavailable(conn->error, sizeof(conn->error));
	return NET_TLS_ERROR;
}

net_tls_status_t
net_tls_conn_read(net_tls_conn_t* conn, void* buf, int size, int* len)
{
	(void)buf;
	(void)size;
	if (len)
		*len = 0;
	tls_unavailable(conn->error, sizeof(conn->error));
	return NET_TLS_ERROR;
}

net_tls_status_t
net_tls_conn_write(net_tls_conn_t* conn, const void* buf, int size, int* len)
{
	(void)buf;
	(void)size;
	if (len)
		*len = 0;
	tls_unavailable(conn->error, sizeof(conn->error));
	return NET_TLS_ERROR;
}
