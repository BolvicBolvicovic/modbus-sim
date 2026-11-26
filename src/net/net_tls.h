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
/** @brief OpenSSL connection, forward-declared so callers need no OpenSSL headers. */
typedef struct ssl_st SSL;
typedef enum net_tls_version_t net_tls_version_t;
typedef enum net_tls_status_t net_tls_status_t;
typedef struct net_tls_config_t net_tls_config_t;
typedef struct net_tls_conn_t net_tls_conn_t;
typedef struct net_tls_ctx_t net_tls_ctx_t;

/** @brief Maximum number of TLS sessions. */
enum
{
	NET_TLS_MAX_SESSIONS = 32,	/**< matches the TCP servers' connection slots */
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
	net_tls_conn_t	conns[NET_TLS_MAX_SESSIONS];
};

/** @brief Returns false and fills `err` when OpenSSL cannot set up the server context. */
bool		net_tls_ctx_init(net_tls_ctx_t* ctx, const net_tls_config_t* config,
				 char* err, u32 err_size);
/**
 * @brief Closes any live sessions and frees the OpenSSL context.
 *
 * Idempotent.
 */
void		net_tls_ctx_close(net_tls_ctx_t* ctx);

/**
 * @brief Creates an accept-side session on a nonblocking socket.
 *
 * The handshake deadline is measured from accept; 0 disables it. NULL and `err` on failure.
 */
net_tls_conn_t*	net_tls_conn_new(net_tls_ctx_t* ctx, int fd, u64 handshake_deadline_ms,
				 char* err, u32 err_size);
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
 * flags directly.
 */
net_tls_status_t net_tls_conn_handshake(net_tls_conn_t* conn, char* err, u32 err_size);
net_tls_status_t net_tls_conn_read(net_tls_conn_t* conn, void* buf, int size, int* len,
				   char* err, u32 err_size);
net_tls_status_t net_tls_conn_write(net_tls_conn_t* conn, const void* buf, int size, int* len,
				    char* err, u32 err_size);

#endif
