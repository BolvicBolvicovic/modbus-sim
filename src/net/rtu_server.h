/**
 * @file
 * @brief Modbus RTU server on a serial-like byte stream.
 */
#ifndef RTU_SERVER_H
#define RTU_SERVER_H

#include "net/net_common.h"
#include "net/rtu_framer.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>
#include <stdbool.h>

typedef struct device_t device_t;

/**
 * @brief Options for serving Modbus RTU over a serial-like file descriptor.
 *
 * The fd is borrowed (not closed by the server). It should be connected to a PTY, serial port,
 * socketpair, or similar bidirectional byte stream. frame_gap_ms, when non-zero, is an
 * explicit inter-frame silence override. Otherwise baud is used to derive Modbus RTU t3.5; if
 * baud is also 0, RTU_DEFAULT_FRAME_GAP_MS is used. strict_t15 enables best-effort intra-frame
 * t1.5 discard checks in baud-derived auto mode; it is opt-in because read() boundaries do not
 * reflect UART timing. broadcast_execute runs unit-0 requests on every device without a
 * response.
 */
typedef struct net_obs_t net_obs_t;
typedef struct rtu_server_options_t rtu_server_options_t;
struct rtu_server_options_t
{
	int		fd;
	u32		seed;
	u32		frame_gap_ms;	/**< explicit inter-frame silence override; 0 = auto */
	u32		baud;		/**< serial baud rate for RTU timing; 0 = unknown */
	bool		strict_t15;	/**< opt-in: discard partial frames on t1.5 gaps */
	bool		broadcast_execute; /**< opt-in: execute unit-0 requests on all devices */
	device_t**	devices;
	u32		device_count;
	net_obs_t*	obs;		/**< metrics, request logs, HTTP endpoint; NULL = none */
};

/**
 * @brief All state of one RTU server; the caller owns the storage and keeps it at a stable
 *        address between init and close.
 *
 * No global state.
 */
typedef struct rtu_server_t rtu_server_t;
struct rtu_server_t
{
	rtu_server_options_t	options;
	rtu_framer_ctx_t	ctx;
	rng_t			rng;
	rtu_framer_t		framer;
	bool			read_closed;
	bool			open;
	volatile sig_atomic_t	stop;
};

/**
 * @brief Resets *server for options->fd (borrowed, not closed).
 *
 * Returns 0.
 */
s32	rtu_server_init(rtu_server_t* server, const rtu_server_options_t* options);

/**
 * @brief One poll iteration, waiting at most max_wait_ms (-1 = until the next frame/response
 *        deadline, at most 1 s; 0 = nonblocking).
 *
 * Returns NET_STEP_CONTINUE; NET_STEP_DONE after rtu_server_stop or when the peer closed the
 * fd; NET_STEP_ERROR if poll failed.
 */
s32	rtu_server_step(rtu_server_t* server, s32 max_wait_ms);

/**
 * @brief async-signal-safe; only sets a flag.
 *
 * The next step returns NET_STEP_DONE.
 */
void	rtu_server_stop(rtu_server_t* server);

/**
 * @brief Ends the session (the fd stays the caller's).
 *
 * Idempotent.
 */
void	rtu_server_close(rtu_server_t* server);

/**
 * @brief Init, step until stopped or the peer closes the fd, close.
 *
 * Returns 0 after a clean stop, non-zero on runtime error.
 */
s32	rtu_server_run(rtu_server_t* server, const rtu_server_options_t* options);

#endif
