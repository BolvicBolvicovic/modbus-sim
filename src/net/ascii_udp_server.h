/**
 * @file
 * @brief Modbus ASCII server over UDP datagrams.
 */
#ifndef ASCII_UDP_SERVER_H
#define ASCII_UDP_SERVER_H

#include "modbus/ascii.h"
#include "net/net_common.h"
#include "net/net_udp.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>

typedef struct device_t device_t;
typedef struct net_obs_t net_obs_t;
typedef struct ascii_udp_server_options_t ascii_udp_server_options_t;
typedef struct ascii_udp_server_t ascii_udp_server_t;

/**
 * @brief What the server needs to run.
 *
 * Devices are borrowed, not copied.
 */
struct ascii_udp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	device_t**	devices;
	u32		device_count;
	net_obs_t*	obs;		/**< metrics, request logs, HTTP endpoint; NULL = none */
};

/**
 * @brief All state of one Modbus ASCII-over-UDP server (one CRLF-terminated ASCII frame per
 *        datagram); the caller owns the storage and keeps it at a stable address between
 *        ascii_udp_server_init and ascii_udp_server_close.
 *
 * No process-global state.
 */
struct ascii_udp_server_t
{
	ascii_udp_server_options_t	options;
	net_udp_t		udp;
	rng_t			rng;
	volatile sig_atomic_t	stop;
};

/**
 * @brief Resets *server and binds the socket.
 *
 * Returns 0, or 2 if it could not start.
 */
s32	ascii_udp_server_init(ascii_udp_server_t* server, const ascii_udp_server_options_t* options);

/**
 * @brief One poll iteration, waiting at most max_wait_ms (-1 = until the next delayed response
 *        is due, at most 1 s; 0 = nonblocking).
 *
 * Returns NET_STEP_CONTINUE, or NET_STEP_DONE once ascii_udp_server_stop was called.
 */
s32	ascii_udp_server_step(ascii_udp_server_t* server, s32 max_wait_ms);

/** @brief Asks the server to stop; only sets a flag, so safe from a signal handler or thread. */
void	ascii_udp_server_stop(ascii_udp_server_t* server);

/**
 * @brief Closes the socket.
 *
 * Idempotent after init.
 */
void	ascii_udp_server_close(ascii_udp_server_t* server);

/**
 * @brief Init, step until stopped, close.
 *
 * Returns 0 after a clean stop, non-zero if the server could not start.
 */
s32	ascii_udp_server_run(ascii_udp_server_t* server, const ascii_udp_server_options_t* options);

#endif
