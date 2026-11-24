#include "net/net_tls.h"
#include <stdio.h>

/* Built when WITH_TLS=0: the API stays linkable for the shared net/tcp_server
 * code, but no TLS session can ever be created. Config loading rejects
 * (tls ...) before this is reached; the error text is kept here too so direct
 * callers get the same explanation. */

struct net_tls_conn_t
{
	bool unused;
};

static void
tls_unavailable(char* err, u32 err_size)
{
	if (err && err_size)
		snprintf(err, err_size, "%s", "built without TLS support");
}

net_tls_ctx_t*
net_tls_ctx_create(const net_tls_config_t* config, char* err, u32 err_size)
{
	(void)config;
	tls_unavailable(err, err_size);
	return NULL;
}

void
net_tls_ctx_destroy(net_tls_ctx_t* ctx)
{
	(void)ctx;
}

net_tls_conn_t*
net_tls_conn_new(net_tls_ctx_t* ctx, int fd, u64 handshake_deadline_ms,
		 char* err, u32 err_size)
{
	(void)ctx;
	(void)fd;
	(void)handshake_deadline_ms;
	tls_unavailable(err, err_size);
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

short
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
net_tls_conn_handshake(net_tls_conn_t* conn, char* err, u32 err_size)
{
	(void)conn;
	tls_unavailable(err, err_size);
	return NET_TLS_ERROR;
}

net_tls_status_t
net_tls_conn_read(net_tls_conn_t* conn, void* buf, int size, int* len, char* err, u32 err_size)
{
	(void)conn;
	(void)buf;
	(void)size;
	if (len)
		*len = 0;
	tls_unavailable(err, err_size);
	return NET_TLS_ERROR;
}

net_tls_status_t
net_tls_conn_write(net_tls_conn_t* conn, const void* buf, int size, int* len, char* err, u32 err_size)
{
	(void)conn;
	(void)buf;
	(void)size;
	if (len)
		*len = 0;
	tls_unavailable(err, err_size);
	return NET_TLS_ERROR;
}
