#include "tests/tests_utils/tls_client.h"
#include "util/clock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TLS_CLIENT_TIMEOUT_MS	1000

static char	g_error[256];

static void
set_error_text(const char* text)
{
	snprintf(g_error, sizeof(g_error), "%s", text);
}

static void
save_error(void)
{
	unsigned long code = ERR_get_error();

	if (code)
		ERR_error_string_n(code, g_error, sizeof(g_error));
	else
		snprintf(g_error, sizeof(g_error), "%s", errno ? strerror(errno) : "TLS error");
	if (!g_error[0])
		set_error_text("unknown TLS error");
	ERR_clear_error();
}

static bool
wait_fd(int fd, short events, u32 timeout_ms)
{
	struct pollfd	pfd = { .fd = fd, .events = events };
	int		ready;

	do
		ready = poll(&pfd, 1, (int)timeout_ms);
	while (ready < 0 && errno == EINTR);
	return ready > 0;
}

const char*
tls_client_error(void)
{
	return g_error;
}

bool
tls_client_open_timeout(u16 port, const char* cert, const char* key, const char* ca,
			u32 timeout_ms, tls_client_t* out)
{
	struct sockaddr_in	addr   = { 0 };
	struct timeval		tv     = { .tv_sec = timeout_ms / 1000,
					   .tv_usec = (timeout_ms % 1000) * 1000 };
	SSL_CTX*		ctx    = NULL;
	X509*		peer   = NULL;
	u64		deadline = clock_now_ms() + timeout_ms;
	bool		handshake_done = false;
	bool		ok     = false;

	out->fd  = -1;
	out->ssl = NULL;
	g_error[0] = '\0';

	ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx)
	{
		save_error();
		goto done;
	}
	if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1)
	{
		save_error();
		goto done;
	}
	if (SSL_CTX_set_cipher_list(ctx, "DEFAULT:!aNULL:!eNULL:!NULL") != 1)
	{
		save_error();
		goto done;
	}
	if (SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1)
	{
		save_error();
		goto done;
	}
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
	if (cert || key)
	{
		if (!cert || !key)
		{
			set_error_text("both certificate and key are required");
			goto done;
		}
		if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
		    SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
		    SSL_CTX_check_private_key(ctx) != 1)
		{
			save_error();
			goto done;
		}
	}

	out->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (out->fd < 0 ||
	    setsockopt(out->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
	    setsockopt(out->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
	{
		save_error();
		goto done;
	}
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(out->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
	{
		save_error();
		close(out->fd);
		out->fd = -1;
		goto done;
	}

	out->ssl = SSL_new(ctx);
	if (!out->ssl)
	{
		save_error();
		goto done;
	}
	SSL_set_connect_state(out->ssl);
	if (SSL_set_fd(out->ssl, out->fd) != 1)
	{
		save_error();
		goto done;
	}
	if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(out->ssl), "127.0.0.1") != 1)
	{
		save_error();
		goto done;
	}

	for (u32 i = 0; i < 50; i++)
	{
		int rc  = SSL_do_handshake(out->ssl);
		int err = SSL_get_error(out->ssl, rc);

		if (rc == 1)
		{
			handshake_done = true;
			break;
		}
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
		{
			save_error();
			goto done;
		}
		u64 now       = clock_now_ms();
		u32 remaining = now < deadline ? (u32)(deadline - now) : 0;

		if (!wait_fd(out->fd, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, remaining))
		{
			set_error_text("TLS handshake timed out");
			goto done;
		}
	}
	if (!handshake_done)
	{
		set_error_text("TLS handshake did not complete");
		goto done;
	}
	if (SSL_get_verify_result(out->ssl) != X509_V_OK)
	{
		snprintf(g_error, sizeof(g_error), "server certificate verification failed: %s",
			 X509_verify_cert_error_string(SSL_get_verify_result(out->ssl)));
		goto done;
	}
	peer = SSL_get1_peer_certificate(out->ssl);
	if (!peer)
	{
		set_error_text("server sent no certificate");
		goto done;
	}
	ok = true;

done:
	X509_free(peer);
	SSL_CTX_free(ctx);
	if (!ok)
		tls_client_close(out);
	return ok;
}

bool
tls_client_open(u16 port, const char* cert, const char* key, const char* ca,
		tls_client_t* out)
{
	return tls_client_open_timeout(port, cert, key, ca, TLS_CLIENT_TIMEOUT_MS, out);
}

void
tls_client_close(tls_client_t* client)
{
	if (client->ssl)
	{
		SSL_free(client->ssl);
		client->ssl = NULL;
	}
	if (client->fd >= 0)
	{
		close(client->fd);
		client->fd = -1;
	}
}

bool
tls_client_write_all(tls_client_t* client, const u8* buf, u32 len)
{
	g_error[0] = '\0';
	while (len > 0)
	{
		int rc  = SSL_write(client->ssl, buf, (int)len);
		int err = SSL_get_error(client->ssl, rc);

		if (rc > 0)
		{
			buf += rc;
			len -= (u32)rc;
			continue;
		}
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
		{
			save_error();
			return false;
		}
		if (!wait_fd(client->fd, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
			     TLS_CLIENT_TIMEOUT_MS))
		{
			set_error_text("TLS write timed out");
			return false;
		}
	}
	return true;
}

bool
tls_client_write_record(tls_client_t* client, const u8* buf, u32 len)
{
	g_error[0] = '\0';
	for (;;)
	{
		int rc  = SSL_write(client->ssl, buf, (int)len);
		int err = SSL_get_error(client->ssl, rc);

		if (rc == (int)len)
			return true;
		if (rc > 0)
		{
			set_error_text("TLS write was split into multiple records");
			return false;
		}
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
		{
			save_error();
			return false;
		}
		if (!wait_fd(client->fd, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
			     TLS_CLIENT_TIMEOUT_MS))
		{
			set_error_text("TLS write timed out");
			return false;
		}
	}
}

bool
tls_client_wait_close_notify(tls_client_t* client, u32 timeout_ms)
{
	u64		deadline = clock_now_ms() + timeout_ms;
	u8		byte;

	g_error[0] = '\0';
	for (;;)
	{
		int	rc;
		int	err;
		u64	now;
		u32	remaining;

		ERR_clear_error();
		rc = SSL_read(client->ssl, &byte, 1);
		err = SSL_get_error(client->ssl, rc);
		if (rc <= 0 && err == SSL_ERROR_ZERO_RETURN)
			return true;
		if (rc > 0)
		{
			set_error_text("unexpected TLS data before close_notify");
			return false;
		}
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
		{
			save_error();
			return false;
		}
		now = clock_now_ms();
		if (now >= deadline)
		{
			set_error_text("TLS close_notify timed out");
			return false;
		}
		remaining = (u32)(deadline - now);
		if (!wait_fd(client->fd, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, remaining))
		{
			set_error_text("TLS close_notify timed out");
			return false;
		}
	}
}

bool
tls_client_read_exact(tls_client_t* client, u8* buf, u32 len)
{
	g_error[0] = '\0';
	while (len > 0)
	{
		int rc  = SSL_read(client->ssl, buf, (int)len);
		int err = SSL_get_error(client->ssl, rc);

		if (rc > 0)
		{
			buf += rc;
			len -= (u32)rc;
			continue;
		}
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
		{
			save_error();
			return false;
		}
		if (!wait_fd(client->fd, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
			     TLS_CLIENT_TIMEOUT_MS))
		{
			set_error_text("TLS read timed out");
			return false;
		}
	}
	return true;
}
