#ifndef RTU_SERVER_H
#define RTU_SERVER_H

#include "vlib/c_types.h"
#include "device/device.h"

/* Name: rtu_server_options_t
 * Description: options for serving Modbus RTU over a serial-like file descriptor.
 * The fd is borrowed (not closed by the server). It should be connected to a
 * PTY, serial port, socketpair, or similar bidirectional byte stream.
 * */
typedef struct rtu_server_options_t rtu_server_options_t;
struct rtu_server_options_t
{
	int		fd;
	u32		seed;
	u32		frame_gap_ms;	/* inter-frame silence; 0 = default (20 ms) */
	device_t**	devices;
	u32		device_count;
};

/* Name: rtu_gap_elapsed
 * Description: true when at least `gap_ms` elapsed between `last_rx_ms` and
 * `now_ms`. Safe when the clock reading is older than the last receive (for
 * example because they were sampled at different points), which must not look
 * like an elapsed gap.
 * */
bool	rtu_gap_elapsed(u64 now_ms, u64 last_rx_ms, u32 gap_ms);

/* Name: rtu_server_run
 * Description: serves Modbus RTU on options->fd until SIGINT or SIGTERM, or until
 * the peer closes the fd. Single thread, poll-based like the TCP server.
 * Returns 0 after a clean stop, non-zero on startup/runtime error.
 * */
s32	rtu_server_run(const rtu_server_options_t* options);

#endif
