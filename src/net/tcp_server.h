/**
 * @file
 * @brief Modbus TCP (MBAP) server, optionally TLS-wrapped.
 */
#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "modbus/mbap.h"
#include "net/net_common.h"
#include "net/net_tcp_pool.h"
#include "net/net_tls.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>

enum
{
	TCP_SERVER_MAX_CONNECTIONS = 32,
};

typedef struct device_t device_t;

/**
 * @brief What the server needs to run.
 *
 * Devices are borrowed, not copied.
 */
typedef struct net_obs_t net_obs_t;
typedef struct tcp_server_options_t tcp_server_options_t;
struct tcp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	u32		idle_timeout_ms;	/**< close idle connections after N ms; 0 = disabled */
	bool		tls_enabled;		/**< wrap MBAP in TLS; borrowed settings from config_t */
	u32		tls_handshake_timeout_ms; /**< TLS only: close handshakes older than this; 0 = disabled */
	net_tls_config_t tls;		/**< borrowed TLS settings, used when tls_enabled */
	device_t**	devices;
	u32		device_count;
	net_obs_t*	obs;		/**< metrics, request logs, HTTP endpoint; NULL = none */
};

/**
 * @brief One client connection.
 *
 * A connection handles one request at a time: while a response is waiting to be sent (tx_len >
 * 0) no new request is read.
 */
typedef struct tcp_conn_t tcp_conn_t;
struct tcp_conn_t
{
	net_tcp_conn_t	base;
	u8		rx[MBAP_MAX_FRAME];
	u32		rx_len;
	u8		tx[MBAP_MAX_FRAME];
	u32		tx_len;
	u32		tx_sent;
	u64		tx_due_ms;	/**< do not send before this time (delay fault) */
	net_tls_conn_t*	tls;		/**< NULL for plaintext connections; points into server->tls */
};

/**
 * @brief All state of one Modbus TCP server; the caller owns the storage (stack, static or
 *        arena) and must keep it at a stable address between tcp_server_init and tcp_server_close.
 *
 * No process-global state.
 */
typedef struct tcp_server_t tcp_server_t;
struct tcp_server_t
{
	tcp_server_options_t	options;
	net_tls_ctx_t		tls;		/**< ssl_ctx is NULL when TLS is off */
	net_tcp_pool_t		pool;
	rng_t			rng;
	tcp_conn_t		conns[TCP_SERVER_MAX_CONNECTIONS];
	volatile sig_atomic_t	stop;
};

/**
 * @brief Resets *server, sets up TLS (if enabled) and the listener.
 *
 * Returns 0, or 2 if it could not start (nothing is left open then).
 */
s32	tcp_server_init(tcp_server_t* server, const tcp_server_options_t* options);

/**
 * @brief One poll iteration, waiting at most max_wait_ms (-1 = until the server's own next
 *        deadline, at most 1 s; 0 = nonblocking).
 *
 * A delayed response never stalls other clients. Returns NET_STEP_CONTINUE, or NET_STEP_DONE
 * once tcp_server_stop was called.
 */
s32	tcp_server_step(tcp_server_t* server, s32 max_wait_ms);

/**
 * @brief Asks the server to finish; only sets a flag, so it is safe from a signal handler or
 *        another thread.
 *
 * The next step returns NET_STEP_DONE.
 */
void	tcp_server_stop(tcp_server_t* server);

/**
 * @brief Closes clients, the listener and TLS.
 *
 * Idempotent after init.
 */
void	tcp_server_close(tcp_server_t* server);

/**
 * @brief Init, step until stopped, close.
 *
 * The binary stops it from its signal handler; no signal handlers are installed here. Returns
 * 0 after a clean stop, non-zero if the server could not start.
 */
s32	tcp_server_run(tcp_server_t* server, const tcp_server_options_t* options);

#endif
