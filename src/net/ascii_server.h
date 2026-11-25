#ifndef ASCII_SERVER_H
#define ASCII_SERVER_H

#include "modbus/ascii.h"
#include "net/net_common.h"
#include "util/rng.h"
#include "vlib/c_types.h"
#include <signal.h>
#include <stdbool.h>

typedef struct device_t device_t;

/* Name: ascii_server_options_t
 * Description: options for serving Modbus ASCII over a serial-like file
 * descriptor. The fd is borrowed (not closed by the server). It should be
 * connected to a PTY, serial port, socketpair, or similar bidirectional byte
 * stream. ASCII framing is delimiter-based (CRLF), so there is no baud,
 * frame-gap, or t1.5 setting here.
 * */
typedef struct ascii_server_options_t ascii_server_options_t;
struct ascii_server_options_t
{
	int		fd;
	u32		seed;
	device_t**	devices;
	u32		device_count;
};

/* Name: ascii_server_t
 * Description: all state of one ASCII server; the caller owns the storage and
 * keeps it at a stable address between init and close. No global state.
 * */
typedef struct ascii_server_t ascii_server_t;
struct ascii_server_t
{
	ascii_server_options_t	options;
	rng_t			rng;
	u8			rx[MODBUS_ASCII_MAX_FRAME];
	u32			rx_len;
	u64			last_rx_ms;
	pending_response_t	pending;
	bool			read_closed;
	bool			open;
	volatile sig_atomic_t	stop;
};

/* Name: ascii_server_init
 * Description: resets *server for options->fd (borrowed, not closed). Returns 0.
 * */
s32	ascii_server_init(ascii_server_t* server, const ascii_server_options_t* options);

/* Name: ascii_server_step
 * Description: one poll iteration, waiting at most max_wait_ms (-1 = until the
 * next response deadline, at most 1 s; 0 = nonblocking). Returns
 * NET_STEP_CONTINUE; NET_STEP_DONE after ascii_server_stop or when the peer
 * closed the fd; NET_STEP_ERROR if poll failed.
 * */
s32	ascii_server_step(ascii_server_t* server, s32 max_wait_ms);

/* Name: ascii_server_stop
 * Description: async-signal-safe; only sets a flag. The next step returns NET_STEP_DONE.
 * */
void	ascii_server_stop(ascii_server_t* server);

/* Name: ascii_server_close
 * Description: ends the session (the fd stays the caller's). Idempotent.
 * */
void	ascii_server_close(ascii_server_t* server);

/* Name: ascii_server_run
 * Description: init, step until stopped or the peer closes the fd, close.
 * Returns 0 after a clean stop, non-zero on runtime error.
 * */
s32	ascii_server_run(ascii_server_t* server, const ascii_server_options_t* options);

#endif
