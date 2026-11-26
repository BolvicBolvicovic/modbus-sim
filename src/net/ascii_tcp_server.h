/**
 * @file
 * @brief Modbus ASCII server over TCP connections.
 */
#ifndef ASCII_TCP_SERVER_H
#define ASCII_TCP_SERVER_H

#include "modbus/ascii.h"
#include "net/net_common.h"
#include "net/net_tcp_pool.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>
#include <stdbool.h>

typedef struct device_t device_t;
typedef struct net_obs_t net_obs_t;
typedef struct ascii_tcp_server_options_t ascii_tcp_server_options_t;
typedef struct ascii_tcp_conn_t ascii_tcp_conn_t;
typedef struct ascii_tcp_server_t ascii_tcp_server_t;

/** @brief Limits of the ASCII-over-TCP server. */
enum
{
	ASCII_TCP_MAX_CONNECTIONS      = 32,
	ASCII_TCP_INTERCHAR_TIMEOUT_MS = 1000,
};

/**
 * @brief Modbus ASCII-over-TCP server options.
 *
 * Devices are borrowed, not copied. Raw ASCII frames (':' + hex [addr][PDU][LRC] + CR LF) are
 * carried over TCP byte streams; this is not MBAP framing. No serial line options apply.
 */
struct ascii_tcp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	u32		idle_timeout_ms;
	device_t**	devices;
	u32		device_count;
	net_obs_t*	obs;		/**< metrics, request logs, HTTP endpoint; NULL = none */
};

/** @brief One ASCII-over-TCP client: its connection and receive buffer. */
struct ascii_tcp_conn_t
{
	net_tcp_conn_t	base;
	u8		rx[MODBUS_ASCII_MAX_FRAME];
	u32		rx_len;
	u64		last_rx_ms;
	pending_response_t pending;
	bool		read_closed;
};

/**
 * @brief All state of one ASCII-over-TCP server; the caller owns the storage and keeps it at a
 *        stable address between init and close.
 */
struct ascii_tcp_server_t
{
	ascii_tcp_server_options_t	options;
	net_tcp_pool_t			pool;
	rng_t				rng;
	ascii_tcp_conn_t		conns[ASCII_TCP_MAX_CONNECTIONS];
	volatile sig_atomic_t		stop;
};

/**
 * @brief Resets *server and opens the listener.
 *
 * Returns 0, or 2 if it could not start.
 */
s32	ascii_tcp_server_init(ascii_tcp_server_t* server, const ascii_tcp_server_options_t* options);

/**
 * @brief One poll iteration, waiting at most max_wait_ms (-1 = until the next deadline, at
 *        most 1 s; 0 = nonblocking).
 *
 * Returns NET_STEP_CONTINUE, NET_STEP_DONE after ascii_tcp_server_stop, or NET_STEP_ERROR if
 * poll failed.
 */
s32	ascii_tcp_server_step(ascii_tcp_server_t* server, s32 max_wait_ms);

/**
 * @brief async-signal-safe; only sets a flag.
 *
 * The next step returns NET_STEP_DONE.
 */
void	ascii_tcp_server_stop(ascii_tcp_server_t* server);

/**
 * @brief Closes clients and the listener.
 *
 * Idempotent after init.
 */
void	ascii_tcp_server_close(ascii_tcp_server_t* server);

/**
 * @brief Init, step until stopped, close.
 *
 * No signal handlers are installed. Returns 0 after a clean stop, non-zero on startup/runtime
 * error.
 */
s32	ascii_tcp_server_run(ascii_tcp_server_t* server, const ascii_tcp_server_options_t* options);

#endif
