/**
 * @file
 * @brief TLS API for TLS-wrapped Modbus TCP.
 *
 * The build selects either the OpenSSL implementation (net_tls.c) or a no-TLS stub
 * (net_tls_stub.c), so callers do not branch on WITH_TLS. MBAP framing and PDU handling stay
 * in tcp_server.c; this layer keeps OpenSSL headers out of modules that include vlib/macros.h
 * (both headers define __counted_by).
 */
#ifndef NET_TLS_H
#define NET_TLS_H

#include "build_config.h"
#include "vlib/c_types.h"
#include "os/os_wait.h"
#include <stdbool.h>

/** @brief OpenSSL TLS context, forward-declared so callers need no OpenSSL headers. */
typedef struct ssl_ctx_st SSL_CTX;
/** @brief OpenSSL BIO method, forward-declared so callers need no OpenSSL headers. */
typedef struct bio_method_st BIO_METHOD;
/** @brief OpenSSL connection, forward-declared so callers need no OpenSSL headers. */
typedef struct ssl_st SSL;
typedef enum net_tls_version_t net_tls_version_t;
typedef enum net_tls_status_t net_tls_status_t;
typedef struct net_tls_config_t net_tls_config_t;
typedef struct net_tls_client_config_t net_tls_client_config_t;
typedef struct net_tls_conn_t net_tls_conn_t;
typedef struct net_tls_ctx_t net_tls_ctx_t;

/** @brief Maximum number of TLS sessions. */
enum
{
	NET_TLS_MAX_SESSIONS = 32,	/**< matches the TCP servers' connection slots */
	NET_TLS_ERROR_MAX    = 192,	/**< size of the error text kept by a context or session */
};

/** @brief Minimum TLS protocol version. */
enum net_tls_version_t
{
	NET_TLS_VERSION_1_2,
	NET_TLS_VERSION_1_3,
};

/** @brief I/O outcome for a nonblocking TLS session. */
enum net_tls_status_t
{
	NET_TLS_OK,
	NET_TLS_WANT_READ,
	NET_TLS_WANT_WRITE,
	NET_TLS_EOF,
	NET_TLS_ERROR,
};

/** @brief TLS settings of a server; strings are borrowed and must outlive the context. */
struct net_tls_config_t
{
	const char*	certificate;
	const char*	private_key;
	const char*	ca_certificate;		/**< NULL when client certs are not verified */
	bool		require_client_certificate;
	net_tls_version_t min_version;
	const char*	tls1_2_ciphers;		/**< NULL = OpenSSL default */
	const char*	tls1_3_ciphersuites;	/**< NULL = OpenSSL default */
};

/** @brief TLS settings of a client; strings are borrowed and only read by net_tls_client_ctx_init(). */
struct net_tls_client_config_t
{
	const char*	ca_certificate;		/**< NULL = the system trust store */
	const char*	certificate;		/**< NULL = no client certificate */
	const char*	private_key;		/**< NULL = the key is in `certificate` */
	bool		verify;			/**< check the server certificate chain and name */
};

/**
 * @brief Per-connection state: SSL object, handshake/want flags and deadline.
 *
 * Only net_tls touches the members; the struct is public so the context can embed its session
 * pool without allocation.
 */
struct net_tls_conn_t
{
	SSL*	ssl;
	bool	handshake_done;
	bool	want_read;
	bool	want_write;
	u64	handshake_deadline_ms;
	char	error[NET_TLS_ERROR_MAX];	/**< text of the last NET_TLS_ERROR of this session */
};

/**
 * @brief Caller-owned TLS context: the OpenSSL context plus this context's own session pool
 *        (no process-global state, so several servers can each have one).
 *
 * Must stay at a stable address while sessions exist. Zero-initialize before init.
 */
struct net_tls_ctx_t
{
	SSL_CTX*	ssl_ctx;
	BIO_METHOD*	bio_method;	/**< socket I/O that never raises SIGPIPE */
	net_tls_conn_t	conns[NET_TLS_MAX_SESSIONS];
	char		error[NET_TLS_ERROR_MAX];	/**< why the last init or conn_new failed */
};

/** @brief Returns false and sets ctx->error when OpenSSL cannot set up the server context. */
bool		net_tls_ctx_init(net_tls_ctx_t* ctx, const net_tls_config_t* config);
/** @brief True when this build has TLS (OpenSSL); false for the no-TLS stub. */
bool		net_tls_supported(void);
/**
 * @brief Sets up a client context (TLS 1.2 or newer); false and ctx->error on failure.
 *
 * Sessions are created with net_tls_client_conn_new().
 */
bool		net_tls_client_ctx_init(net_tls_ctx_t* ctx, const net_tls_client_config_t* config);
/**
 * @brief Creates a connect-side session on a nonblocking, connected socket.
 *
 * With verification on, the certificate must match `server_name` (a DNS name, or an IP literal
 * checked against the certificate's IP addresses); a DNS name is also sent as SNI. NULL and
 * ctx->error on failure.
 */
net_tls_conn_t*	net_tls_client_conn_new(net_tls_ctx_t* ctx, int fd, const char* server_name);

/**
 * @brief Closes any live sessions and frees the OpenSSL context.
 *
 * Idempotent.
 */
void		net_tls_ctx_close(net_tls_ctx_t* ctx);

/**
 * @brief Creates an accept-side session on a nonblocking socket.
 *
 * The handshake deadline is measured from accept; 0 disables it. NULL and ctx->error on failure.
 */
net_tls_conn_t*	net_tls_conn_new(net_tls_ctx_t* ctx, int fd, u64 handshake_deadline_ms);
/** @brief Sends a best-effort close_notify, then frees the session. */
void		net_tls_conn_close(net_tls_conn_t* conn);

bool		net_tls_conn_handshake_done(const net_tls_conn_t* conn);
u64		net_tls_conn_handshake_deadline(const net_tls_conn_t* conn);
/** @brief OS_WAIT_IN/OS_WAIT_OUT bits requested by OpenSSL; 0 when none. */
u32		net_tls_conn_poll_events(const net_tls_conn_t* conn);
int		net_tls_conn_pending(const net_tls_conn_t* conn);
/** @brief Copies the peer certificate subject in RFC2253 form; false when unavailable. */
bool		net_tls_conn_peer_subject(const net_tls_conn_t* conn, char* buf, u32 size);

/**
 * @brief Nonblocking TLS operations.
 *
 * Read/write update the want flags and place the byte count in `len`; callers never touch the
 * flags directly. NET_TLS_ERROR leaves its text in conn->error.
 */
net_tls_status_t net_tls_conn_handshake(net_tls_conn_t* conn);
net_tls_status_t net_tls_conn_read(net_tls_conn_t* conn, void* buf, int size, int* len);
net_tls_status_t net_tls_conn_write(net_tls_conn_t* conn, const void* buf, int size, int* len);

#endif
