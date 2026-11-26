#ifndef RTU_UDP_SERVER_H
#define RTU_UDP_SERVER_H

#include "net/net_common.h"
#include "net/net_udp.h"
#include "net/rtu_framer.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>
#include <stdbool.h>

typedef struct device_t device_t;

/* Name: rtu_udp_server_options_t
 * Description: what the server needs to run. Devices are borrowed, not copied.
 * Each datagram carries exactly one RTU frame, so there is no t3.5/t1.5 timing.
 * broadcast_execute runs unit-0 requests on every device without a response.
 * */
typedef struct rtu_udp_server_options_t rtu_udp_server_options_t;
struct rtu_udp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	bool		broadcast_execute;
	device_t**	devices;
	u32		device_count;
};

/* Name: rtu_udp_server_t
 * Description: all state of one RTU-over-UDP server; the caller owns the
 * storage and keeps it at a stable address between init and close.
 * */
typedef struct rtu_udp_server_t rtu_udp_server_t;
struct rtu_udp_server_t
{
	rtu_udp_server_options_t	options;
	net_udp_t			udp;
	rtu_framer_t			framer;	/* scratch for the reply of one datagram */
	rng_t				rng;
	volatile sig_atomic_t		stop;
};

/* Resets *server and binds the socket. Returns 0, or 2 if it could not start. */
s32	rtu_udp_server_init(rtu_udp_server_t* server, const rtu_udp_server_options_t* options);

/* One poll iteration, waiting at most max_wait_ms (-1 = until the next delayed
 * response is due, at most 1 s; 0 = nonblocking). Returns NET_STEP_CONTINUE, or
 * NET_STEP_DONE once rtu_udp_server_stop was called. */
s32	rtu_udp_server_step(rtu_udp_server_t* server, s32 max_wait_ms);

/* Only sets a flag; safe from a signal handler or another thread. */
void	rtu_udp_server_stop(rtu_udp_server_t* server);

/* Closes the socket. Idempotent after init. */
void	rtu_udp_server_close(rtu_udp_server_t* server);

/* init, step until stopped, close. Returns 0 after a clean stop, non-zero if
 * the server could not start. */
s32	rtu_udp_server_run(rtu_udp_server_t* server, const rtu_udp_server_options_t* options);

#endif
