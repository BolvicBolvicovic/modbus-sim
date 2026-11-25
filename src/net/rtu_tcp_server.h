#ifndef RTU_TCP_SERVER_H
#define RTU_TCP_SERVER_H

#include "net/net_common.h"
#include "net/net_tcp_pool.h"
#include "net/rtu_framer.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>
#include <stdbool.h>

enum
{
	RTU_TCP_MAX_CONNECTIONS = 32,
};

typedef struct device_t device_t;

/* Name: rtu_tcp_server_options_t
 * Description: Modbus RTU-over-TCP server options. Devices are borrowed, not copied.
 * RTU frames (addr+PDU+CRC) are transported over TCP connections.
 * frame_gap_ms/baud follow the same RTU timing rules as the byte-stream RTU server.
 * strict_t15 is intentionally not supported for RTU-over-TCP: TCP read() boundaries
 * do not reflect UART character timing. broadcast_execute runs unit-0 requests on
 * every device without a response.
 * */
typedef struct rtu_tcp_server_options_t rtu_tcp_server_options_t;
struct rtu_tcp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	u32		idle_timeout_ms;
	u32		frame_gap_ms;
	u32		baud;
	bool		broadcast_execute; /* opt-in: execute unit-0 requests on all devices */
	device_t**	devices;
	u32		device_count;
};

typedef struct rtu_tcp_conn_t rtu_tcp_conn_t;
struct rtu_tcp_conn_t
{
	net_tcp_conn_t	base;
	rtu_framer_t	framer;
	bool		read_closed;
};

/* Name: rtu_tcp_server_t
 * Description: all state of one RTU-over-TCP server; the caller owns the
 * storage and keeps it at a stable address between init and close.
 * */
typedef struct rtu_tcp_server_t rtu_tcp_server_t;
struct rtu_tcp_server_t
{
	rtu_tcp_server_options_t	options;
	rtu_framer_ctx_t		ctx;
	net_tcp_pool_t			pool;
	u32				gap_ms;
	rng_t				rng;
	rtu_tcp_conn_t			conns[RTU_TCP_MAX_CONNECTIONS];
	volatile sig_atomic_t		stop;
};

/* Name: rtu_tcp_server_init
 * Description: resets *server and opens the listener. Returns 0, or 2 if it
 * could not start.
 * */
s32	rtu_tcp_server_init(rtu_tcp_server_t* server, const rtu_tcp_server_options_t* options);

/* Name: rtu_tcp_server_step
 * Description: one poll iteration, waiting at most max_wait_ms (-1 = until the
 * next deadline, at most 1 s; 0 = nonblocking). Returns NET_STEP_CONTINUE, or
 * NET_STEP_DONE after rtu_tcp_server_stop.
 * */
s32	rtu_tcp_server_step(rtu_tcp_server_t* server, s32 max_wait_ms);

/* Name: rtu_tcp_server_stop
 * Description: async-signal-safe; only sets a flag. The next step returns NET_STEP_DONE.
 * */
void	rtu_tcp_server_stop(rtu_tcp_server_t* server);

/* Name: rtu_tcp_server_close
 * Description: closes clients and the listener. Idempotent after init.
 * */
void	rtu_tcp_server_close(rtu_tcp_server_t* server);

/* Name: rtu_tcp_server_run
 * Description: init, step until stopped, close. No signal handlers are installed.
 * Returns 0 after a clean stop, non-zero if the server could not start.
 * */
s32	rtu_tcp_server_run(rtu_tcp_server_t* server, const rtu_tcp_server_options_t* options);

#endif
