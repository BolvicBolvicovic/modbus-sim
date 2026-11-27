/**
 * @file
 * @brief OpenSSL implementation of the TLS layer for Modbus TCP.
 */
#include "net/net_tls.h"
#include "os/os_error.h"
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <string.h>

static void
copy_last_error(char* out, u32 size)
{
	unsigned long code = ERR_get_error();

	if (code)
		ERR_error_string_n(code, out, size);
	else
		snprintf(out, size, "%s", os_last_error() ? os_last_error_text() : "unknown error");
	if (!out[0])
		snprintf(out, size, "unknown error");
	ERR_clear_error();
}

static bool
tls_fail(SSL_CTX* ctx, net_tls_ctx_t* out, const char* what)
{
	char detail[NET_TLS_ERROR_MAX];

	copy_last_error(detail, sizeof(detail));
	snprintf(out->error, sizeof(out->error), "%s: %s", what, detail);
	if (ctx)
		SSL_CTX_free(ctx);
	return false;
}

bool
net_tls_ctx_init(net_tls_ctx_t* out, const net_tls_config_t* config)
{
	char			cipher_list[1024];
	int			len;
	long			options = 0;
	int			verify  = SSL_VERIFY_NONE;
	SSL_CTX*		ctx     = NULL;

	memset(out, 0, sizeof(*out));

	if (config->require_client_certificate && !config->ca_certificate)
	{
		snprintf(out->error, sizeof(out->error), "client certificates are required but no CA certificate is configured");
		return false;
	}

	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx)
		return tls_fail(NULL, out, "cannot create TLS context");

	if (SSL_CTX_set_min_proto_version(ctx,
					  config->min_version == NET_TLS_VERSION_1_3 ?
					  TLS1_3_VERSION : TLS1_2_VERSION) != 1)
		return tls_fail(ctx, out, "cannot set minimum TLS version");

#ifdef SSL_OP_NO_SSLv2
	options |= SSL_OP_NO_SSLv2;
#endif
#ifdef SSL_OP_NO_SSLv3
	options |= SSL_OP_NO_SSLv3;
#endif
#ifdef SSL_OP_NO_TLSv1
	options |= SSL_OP_NO_TLSv1;
#endif
#ifdef SSL_OP_NO_TLSv1_1
	options |= SSL_OP_NO_TLSv1_1;
#endif
#ifdef SSL_OP_NO_RENEGOTIATION
	options |= SSL_OP_NO_RENEGOTIATION;
#endif
	/* set_options returns the resulting option mask, not a success flag. */
	(void)SSL_CTX_set_options(ctx, options);

	len = snprintf(cipher_list, sizeof(cipher_list), "%s:!aNULL:!eNULL:!NULL",
		       config->tls1_2_ciphers ? config->tls1_2_ciphers : "DEFAULT");
	if (len < 0 || (size_t)len >= sizeof(cipher_list))
	{
		snprintf(out->error, sizeof(out->error), "TLS 1.2 cipher list is too long");
		SSL_CTX_free(ctx);
		return false;
	}
	if (SSL_CTX_set_cipher_list(ctx, cipher_list) != 1)
		return tls_fail(ctx, out, "invalid TLS 1.2 cipher list");

	if (config->tls1_3_ciphersuites &&
	    SSL_CTX_set_ciphersuites(ctx, config->tls1_3_ciphersuites) != 1)
		return tls_fail(ctx, out, "invalid TLS 1.3 cipher suites");

	if (SSL_CTX_use_certificate_chain_file(ctx, config->certificate) != 1)
		return tls_fail(ctx, out, "cannot load server certificate");
	if (SSL_CTX_use_PrivateKey_file(ctx, config->private_key, SSL_FILETYPE_PEM) != 1)
		return tls_fail(ctx, out, "cannot load server private key");
	if (SSL_CTX_check_private_key(ctx) != 1)
		return tls_fail(ctx, out, "server certificate and private key do not match");

	if (config->ca_certificate)
	{
		STACK_OF(X509_NAME)* ca_names;

		if (SSL_CTX_load_verify_locations(ctx, config->ca_certificate, NULL) != 1)
			return tls_fail(ctx, out, "cannot load CA certificate");
		ca_names = SSL_load_client_CA_file(config->ca_certificate);
		if (!ca_names)
			return tls_fail(ctx, out, "cannot load client CA names");
		SSL_CTX_set_client_CA_list(ctx, ca_names);
		verify = SSL_VERIFY_PEER;
		if (config->require_client_certificate)
			verify |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
	}
	SSL_CTX_set_verify(ctx, verify, NULL);
	SSL_CTX_set_verify_depth(ctx, 4);
	out->ssl_ctx = ctx;
	return true;
}

net_tls_conn_t*
net_tls_conn_new(net_tls_ctx_t* ctx, int fd, u64 handshake_deadline_ms)
{
	net_tls_conn_t* conn = NULL;
	SSL*		ssl;

	for (u32 i = 0; i < NET_TLS_MAX_SESSIONS; i++)
	{
		if (!ctx->conns[i].ssl)
		{
			conn = &ctx->conns[i];
			break;
		}
	}
	if (!conn)
	{
		snprintf(ctx->error, sizeof(ctx->error), "too many TLS sessions");
		return NULL;
	}
	ssl = SSL_new(ctx->ssl_ctx);
	if (!ssl)
	{
		copy_last_error(ctx->error, sizeof(ctx->error));
		return NULL;
	}
	if (SSL_set_fd(ssl, fd) != 1)
	{
		copy_last_error(ctx->error, sizeof(ctx->error));
		SSL_free(ssl);
		return NULL;
	}
	SSL_set_accept_state(ssl);
	conn->ssl                   = ssl;
	conn->handshake_done        = false;
	conn->want_read             = true;
	conn->want_write            = false;
	conn->handshake_deadline_ms = handshake_deadline_ms;
	return conn;
}

static void
ssl_close(SSL* ssl)
{
	/* One nonblocking attempt: a peer that is not reading must not stall other
	 * clients while we try to deliver close_notify. */
	ERR_clear_error();
	(void)SSL_shutdown(ssl);
	SSL_free(ssl);
}

void
net_tls_conn_close(net_tls_conn_t* conn)
{
	if (!conn || !conn->ssl)
		return;
	ssl_close(conn->ssl);
	memset(conn, 0, sizeof(*conn));
}

void
net_tls_ctx_close(net_tls_ctx_t* ctx)
{
	for (u32 i = 0; i < NET_TLS_MAX_SESSIONS; i++)
		net_tls_conn_close(&ctx->conns[i]);
	if (ctx->ssl_ctx)
		SSL_CTX_free(ctx->ssl_ctx);
	ctx->ssl_ctx = NULL;
}

bool
net_tls_conn_handshake_done(const net_tls_conn_t* conn)
{
	return conn && conn->handshake_done;
}

u64
net_tls_conn_handshake_deadline(const net_tls_conn_t* conn)
{
	return conn ? conn->handshake_deadline_ms : 0;
}

u32
net_tls_conn_poll_events(const net_tls_conn_t* conn)
{
	u32 events = 0;

	if (conn && conn->want_read)
		events |= OS_WAIT_IN;
	if (conn && conn->want_write)
		events |= OS_WAIT_OUT;
	return events;
}

int
net_tls_conn_pending(const net_tls_conn_t* conn)
{
	return conn && conn->ssl ? SSL_pending(conn->ssl) : 0;
}

static bool
peer_subject(SSL* ssl, char* buf, u32 size)
{
	X509*	cert;
	BIO*	bio;
	bool	ok = false;

	if (!ssl || size < 2)
		return false;
	buf[0] = '\0';
	cert = SSL_get1_peer_certificate(ssl);
	if (!cert)
		return false;
	bio = BIO_new(BIO_s_mem());
	if (bio &&
	    X509_NAME_print_ex(bio, X509_get_subject_name(cert), 0, XN_FLAG_RFC2253) > 0)
	{
		int len = BIO_read(bio, buf, (int)size - 1);

		if (len > 0)
		{
			buf[len] = '\0';
			ok = true;
		}
	}
	BIO_free(bio);
	X509_free(cert);
	return ok;
}

bool
net_tls_conn_peer_subject(const net_tls_conn_t* conn, char* buf, u32 size)
{
	return conn && peer_subject(conn->ssl, buf, size);
}

static net_tls_status_t
io_status(SSL* ssl, int rc)
{
	int err = SSL_get_error(ssl, rc);

	if (err == SSL_ERROR_NONE)
		return NET_TLS_OK;
	if (err == SSL_ERROR_WANT_READ)
		return NET_TLS_WANT_READ;
	if (err == SSL_ERROR_WANT_WRITE)
		return NET_TLS_WANT_WRITE;
	if (err == SSL_ERROR_ZERO_RETURN)
		return NET_TLS_EOF;
	/* A peer that drops without close_notify can surface as SSL_ERROR_SYSCALL
	 * with an empty error queue and a stale errno; do not report it as a TLS
	 * failure. */
	if (err == SSL_ERROR_SYSCALL && ERR_peek_error() == 0)
		return NET_TLS_EOF;
	return NET_TLS_ERROR;
}

static void
set_wants(net_tls_conn_t* conn, net_tls_status_t status)
{
	conn->want_read  = status == NET_TLS_WANT_READ;
	conn->want_write = status == NET_TLS_WANT_WRITE;
}

net_tls_status_t
net_tls_conn_handshake(net_tls_conn_t* conn)
{
	int rc;
	net_tls_status_t status;

	conn->want_read  = false;
	conn->want_write = false;
	ERR_clear_error();
	rc = SSL_do_handshake(conn->ssl);
	status = io_status(conn->ssl, rc);
	if (status == NET_TLS_OK)
		conn->handshake_done = true;
	if (status == NET_TLS_ERROR)
		copy_last_error(conn->error, sizeof(conn->error));
	set_wants(conn, status);
	return status;
}

net_tls_status_t
net_tls_conn_read(net_tls_conn_t* conn, void* buf, int size, int* len)
{
	int rc;
	net_tls_status_t status;

	conn->want_read  = false;
	conn->want_write = false;
	ERR_clear_error();
	rc = SSL_read(conn->ssl, buf, size);
	status = io_status(conn->ssl, rc);
	*len = rc > 0 ? rc : 0;
	if (status == NET_TLS_ERROR)
		copy_last_error(conn->error, sizeof(conn->error));
	set_wants(conn, status);
	return status;
}

net_tls_status_t
net_tls_conn_write(net_tls_conn_t* conn, const void* buf, int size, int* len)
{
	int rc;
	net_tls_status_t status;

	conn->want_read  = false;
	conn->want_write = false;
	ERR_clear_error();
	rc = SSL_write(conn->ssl, buf, size);
	status = io_status(conn->ssl, rc);
	*len = rc > 0 ? rc : 0;
	if (status == NET_TLS_ERROR)
		copy_last_error(conn->error, sizeof(conn->error));
	set_wants(conn, status);
	return status;
}
