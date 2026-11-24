#include "net/net_tls.h"
#include <errno.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#define TLS_SHUTDOWN_WAIT_MS	50
#define TLS_SHUTDOWN_POLL_MS	5
#define TLS_SHUTDOWN_ATTEMPTS	(TLS_SHUTDOWN_WAIT_MS / TLS_SHUTDOWN_POLL_MS)

static void
copy_last_error(char* out, u32 size)
{
	unsigned long code = ERR_get_error();

	if (code)
		ERR_error_string_n(code, out, size);
	else
		snprintf(out, size, "%s", errno ? strerror(errno) : "unknown error");
	if (!out[0])
		snprintf(out, size, "unknown error");
	ERR_clear_error();
}

static net_tls_ctx_t*
tls_fail(net_tls_ctx_t* ctx, char* err, u32 err_size, const char* what)
{
	char detail[256];

	copy_last_error(detail, sizeof(detail));
	snprintf(err, err_size, "%s: %s", what, detail);
	if (ctx)
		net_tls_ctx_destroy(ctx);
	return NULL;
}

net_tls_ctx_t*
net_tls_ctx_create(const net_tls_config_t* config, char* err, u32 err_size)
{
	char			cipher_list[1024];
	int			len;
	long			options = 0;
	int			verify  = SSL_VERIFY_NONE;
	net_tls_ctx_t*		ctx     = NULL;

	if (config->require_client_certificate && !config->ca_certificate)
	{
		snprintf(err, err_size, "client certificates are required but no CA certificate is configured");
		return NULL;
	}

	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx)
		return tls_fail(NULL, err, err_size, "cannot create TLS context");

	if (SSL_CTX_set_min_proto_version(ctx,
					  config->min_version == NET_TLS_VERSION_1_3 ?
					  TLS1_3_VERSION : TLS1_2_VERSION) != 1)
		return tls_fail(ctx, err, err_size, "cannot set minimum TLS version");

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
		snprintf(err, err_size, "TLS 1.2 cipher list is too long");
		net_tls_ctx_destroy(ctx);
		return NULL;
	}
	if (SSL_CTX_set_cipher_list(ctx, cipher_list) != 1)
		return tls_fail(ctx, err, err_size, "invalid TLS 1.2 cipher list");

	if (config->tls1_3_ciphersuites &&
	    SSL_CTX_set_ciphersuites(ctx, config->tls1_3_ciphersuites) != 1)
		return tls_fail(ctx, err, err_size, "invalid TLS 1.3 cipher suites");

	if (SSL_CTX_use_certificate_chain_file(ctx, config->certificate) != 1)
		return tls_fail(ctx, err, err_size, "cannot load server certificate");
	if (SSL_CTX_use_PrivateKey_file(ctx, config->private_key, SSL_FILETYPE_PEM) != 1)
		return tls_fail(ctx, err, err_size, "cannot load server private key");
	if (SSL_CTX_check_private_key(ctx) != 1)
		return tls_fail(ctx, err, err_size, "server certificate and private key do not match");

	if (config->ca_certificate)
	{
		STACK_OF(X509_NAME)* ca_names;

		if (SSL_CTX_load_verify_locations(ctx, config->ca_certificate, NULL) != 1)
			return tls_fail(ctx, err, err_size, "cannot load CA certificate");
		ca_names = SSL_load_client_CA_file(config->ca_certificate);
		if (!ca_names)
			return tls_fail(ctx, err, err_size, "cannot load client CA names");
		SSL_CTX_set_client_CA_list(ctx, ca_names);
		verify = SSL_VERIFY_PEER;
		if (config->require_client_certificate)
			verify |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
	}
	SSL_CTX_set_verify(ctx, verify, NULL);
	SSL_CTX_set_verify_depth(ctx, 4);
	return ctx;
}

void
net_tls_ctx_destroy(net_tls_ctx_t* ctx)
{
	if (ctx)
		SSL_CTX_free(ctx);
}

SSL*
net_tls_session_new(net_tls_ctx_t* ctx, int fd, char* err, u32 err_size)
{
	SSL* ssl = SSL_new(ctx);

	if (!ssl)
	{
		copy_last_error(err, err_size);
		return NULL;
	}
	if (SSL_set_fd(ssl, fd) != 1)
	{
		copy_last_error(err, err_size);
		SSL_free(ssl);
		return NULL;
	}
	SSL_set_accept_state(ssl);
	return ssl;
}

bool
net_tls_peer_subject(SSL* ssl, char* buf, u32 size)
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

void
net_tls_session_close(SSL* ssl)
{
	if (!ssl)
		return;

	/* The socket is nonblocking: keep trying while shutdown needs I/O, but never
	 * wait for a peer that is not reading or sending. */
	for (int i = 0; i < TLS_SHUTDOWN_ATTEMPTS; i++)
	{
		struct pollfd	pfd = { .fd = SSL_get_fd(ssl) };
		int		rc;
		int		err;
		int		ready;

		ERR_clear_error();
		rc = SSL_shutdown(ssl);
		if (rc >= 0)
			break;
		err = SSL_get_error(ssl, rc);
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
			break;
		pfd.events = err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
		ready = poll(&pfd, 1, TLS_SHUTDOWN_POLL_MS);
		if (ready < 0 && errno != EINTR)
			break;
	}
	SSL_free(ssl);
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
	return NET_TLS_ERROR;
}

net_tls_status_t
net_tls_handshake(SSL* ssl, char* err, u32 err_size)
{
	int rc;
	int status;

	ERR_clear_error();
	rc = SSL_do_handshake(ssl);
	status = io_status(ssl, rc);

	if (status == NET_TLS_ERROR)
		copy_last_error(err, err_size);
	return status;
}

net_tls_status_t
net_tls_read(SSL* ssl, void* buf, int size, int* len, char* err, u32 err_size)
{
	int rc;
	int status;

	ERR_clear_error();
	rc = SSL_read(ssl, buf, size);
	status = io_status(ssl, rc);

	*len = rc > 0 ? rc : 0;
	if (status == NET_TLS_ERROR)
		copy_last_error(err, err_size);
	return status;
}

net_tls_status_t
net_tls_write(SSL* ssl, const void* buf, int size, int* len, char* err, u32 err_size)
{
	int rc;
	int status;

	ERR_clear_error();
	rc = SSL_write(ssl, buf, size);
	status = io_status(ssl, rc);

	*len = rc > 0 ? rc : 0;
	if (status == NET_TLS_ERROR)
		copy_last_error(err, err_size);
	return status;
}

int
net_tls_pending(SSL* ssl)
{
	return SSL_pending(ssl);
}
