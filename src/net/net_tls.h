#ifndef NET_TLS_H
#define NET_TLS_H

#include "build_config.h"
#include "vlib/c_types.h"
#include <stdbool.h>

/* TLS API for TLS-wrapped Modbus TCP. The build selects either the OpenSSL
 * implementation (net_tls.c) or a no-TLS stub (net_tls_stub.c), so callers do
 * not branch on WITH_TLS. MBAP framing and PDU handling stay in tcp_server.c;
 * this layer keeps OpenSSL headers out of modules that include vlib/macros.h
 * (both headers define __counted_by). */

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

typedef enum net_tls_version_t net_tls_version_t;
enum net_tls_version_t
{
	NET_TLS_VERSION_1_2,
	NET_TLS_VERSION_1_3,
};

/* I/O outcome for a nonblocking TLS session. */
typedef enum net_tls_status_t net_tls_status_t;
enum net_tls_status_t
{
	NET_TLS_OK,
	NET_TLS_WANT_READ,
	NET_TLS_WANT_WRITE,
	NET_TLS_EOF,
	NET_TLS_ERROR,
};

/* All strings are borrowed from the config and must outlive the context. */
typedef struct net_tls_config_t net_tls_config_t;
struct net_tls_config_t
{
	const char*	certificate;
	const char*	private_key;
	const char*	ca_certificate;		/* NULL when client certs are not verified */
	bool		require_client_certificate;
	net_tls_version_t min_version;
	const char*	tls1_2_ciphers;		/* NULL = OpenSSL default */
	const char*	tls1_3_ciphersuites;	/* NULL = OpenSSL default */
};

typedef SSL_CTX net_tls_ctx_t;

/* Returns NULL and fills `err` when OpenSSL cannot set up the server context. */
net_tls_ctx_t*	net_tls_ctx_create(const net_tls_config_t* config, char* err, u32 err_size);
void		net_tls_ctx_destroy(net_tls_ctx_t* ctx);

/* Creates an accept-side SSL object on a nonblocking socket. */
SSL*		net_tls_session_new(net_tls_ctx_t* ctx, int fd, char* err, u32 err_size);
/* Copies the peer certificate subject in RFC2253 form; false when there is no peer
 * certificate or it cannot be rendered. */
bool		net_tls_peer_subject(SSL* ssl, char* buf, u32 size);
/* Sends a best-effort close_notify, waiting at most a few tens of ms, then frees the session. */
void		net_tls_session_close(SSL* ssl);

/* Nonblocking TLS operations. For read/write, `len` receives the byte count on OK. */
net_tls_status_t net_tls_handshake(SSL* ssl, char* err, u32 err_size);
net_tls_status_t net_tls_read(SSL* ssl, void* buf, int size, int* len, char* err, u32 err_size);
net_tls_status_t net_tls_write(SSL* ssl, const void* buf, int size, int* len, char* err, u32 err_size);
int		net_tls_pending(SSL* ssl);

#endif
