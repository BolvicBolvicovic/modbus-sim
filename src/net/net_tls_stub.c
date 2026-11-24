#include "net/net_tls.h"
#include <stdio.h>

/* Built when WITH_TLS=0: the API stays linkable for the shared net/tcp_server
 * code, but no TLS session can ever be created. Config loading rejects
 * (tls ...) before this is reached; the error text is kept here too so direct
 * callers get the same explanation. */
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

SSL*
net_tls_session_new(net_tls_ctx_t* ctx, int fd, char* err, u32 err_size)
{
	(void)ctx;
	(void)fd;
	tls_unavailable(err, err_size);
	return NULL;
}

bool
net_tls_peer_subject(SSL* ssl, char* buf, u32 size)
{
	(void)ssl;
	if (buf && size)
		buf[0] = '\0';
	return false;
}

void
net_tls_session_close(SSL* ssl)
{
	(void)ssl;
}

net_tls_status_t
net_tls_handshake(SSL* ssl, char* err, u32 err_size)
{
	(void)ssl;
	tls_unavailable(err, err_size);
	return NET_TLS_ERROR;
}

net_tls_status_t
net_tls_read(SSL* ssl, void* buf, int size, int* len, char* err, u32 err_size)
{
	(void)ssl;
	(void)buf;
	(void)size;
	if (len)
		*len = 0;
	tls_unavailable(err, err_size);
	return NET_TLS_ERROR;
}

net_tls_status_t
net_tls_write(SSL* ssl, const void* buf, int size, int* len, char* err, u32 err_size)
{
	(void)ssl;
	(void)buf;
	(void)size;
	if (len)
		*len = 0;
	tls_unavailable(err, err_size);
	return NET_TLS_ERROR;
}

int
net_tls_pending(SSL* ssl)
{
	(void)ssl;
	return 0;
}
