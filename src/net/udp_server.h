/**
 * @file
 * @brief Modbus UDP server.
 */
#ifndef UDP_SERVER_H
#define UDP_SERVER_H

#include "modbus/mbap.h"
#include "net/net_common.h"
#include "net/net_udp.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>

typedef struct device_t device_t;

/**
 * @brief What the server needs to run.
 *
 * Devices are borrowed, not copied.
 */
typedef struct udp_server_options_t udp_server_options_t;
struct udp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	device_t**	devices;
	u32		device_count;
};

/**
 * @brief All state of one Modbus UDP server (one MBAP ADU per datagram); the caller owns the
 *        storage and keeps it at a stable address between udp_server_init and udp_server_close.
 *
 * No process-global state.
 */
typedef struct udp_server_t udp_server_t;
struct udp_server_t
{
	udp_server_options_t	options;
	net_udp_t		udp;
	rng_t			rng;
	volatile sig_atomic_t	stop;
};

/**
 * @brief Resets *server and binds the socket.
 *
 * Returns 0, or 2 if it could not start.
 */
s32	udp_server_init(udp_server_t* server, const udp_server_options_t* options);

/**
 * @brief One poll iteration, waiting at most max_wait_ms (-1 = until the next delayed response
 *        is due, at most 1 s; 0 = nonblocking).
 *
 * Returns NET_STEP_CONTINUE, or NET_STEP_DONE once udp_server_stop was called.
 */
s32	udp_server_step(udp_server_t* server, s32 max_wait_ms);

/** @brief Asks the server to stop; only sets a flag, so safe from a signal handler or thread. */
void	udp_server_stop(udp_server_t* server);

/**
 * @brief Closes the socket.
 *
 * Idempotent after init.
 */
void	udp_server_close(udp_server_t* server);

/**
 * @brief Init, step until stopped, close.
 *
 * Returns 0 after a clean stop, non-zero if the server could not start.
 */
s32	udp_server_run(udp_server_t* server, const udp_server_options_t* options);

#endif
