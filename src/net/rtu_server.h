#ifndef RTU_SERVER_H
#define RTU_SERVER_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct device_t device_t;

/* Name: rtu_server_options_t
 * Description: options for serving Modbus RTU over a serial-like file descriptor.
 * The fd is borrowed (not closed by the server). It should be connected to a
 * PTY, serial port, socketpair, or similar bidirectional byte stream.
 * frame_gap_ms, when non-zero, is an explicit inter-frame silence override.
 * Otherwise baud is used to derive Modbus RTU t3.5; if baud is also 0,
 * RTU_DEFAULT_FRAME_GAP_MS is used.
 * strict_t15 enables best-effort intra-frame t1.5 discard checks in baud-derived
 * auto mode; it is opt-in because read() boundaries do not reflect UART timing.
 * */
typedef struct rtu_server_options_t rtu_server_options_t;
struct rtu_server_options_t
{
	int		fd;
	u32		seed;
	u32		frame_gap_ms;	/* explicit inter-frame silence override; 0 = auto */
	u32		baud;		/* serial baud rate for RTU timing; 0 = unknown */
	bool		strict_t15;	/* opt-in: discard partial frames on t1.5 gaps */
	device_t**	devices;
	u32		device_count;
};

/* Name: rtu_server_run
 * Description: serves Modbus RTU on options->fd until SIGINT or SIGTERM, or until
 * the peer closes the fd. Single thread, poll-based like the TCP server.
 * Returns 0 after a clean stop, non-zero on startup/runtime error.
 * */
s32	rtu_server_run(const rtu_server_options_t* options);

#endif
