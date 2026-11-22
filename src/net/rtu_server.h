#ifndef RTU_SERVER_H
#define RTU_SERVER_H

#include "vlib/c_types.h"
#include <stdbool.h>

#define RTU_DEFAULT_FRAME_GAP_MS	20
#define RTU_BAUD_FIXED_THRESHOLD	19200U
#define RTU_INTERCHAR_GAP_US_HIGH	750U	/* t1.5 at >19200 baud */
#define RTU_INTERFRAME_GAP_US_HIGH	1750U	/* t3.5 at >19200 baud */

typedef struct device_t device_t;

/* Name: rtu_server_options_t
 * Description: options for serving Modbus RTU over a serial-like file descriptor.
 * The fd is borrowed (not closed by the server). It should be connected to a
 * PTY, serial port, socketpair, or similar bidirectional byte stream.
 * frame_gap_ms, when non-zero, is an explicit inter-frame silence override.
 * Otherwise baud is used to derive Modbus RTU t3.5; if baud is also 0,
 * RTU_DEFAULT_FRAME_GAP_MS is used.
 * */
typedef struct rtu_server_options_t rtu_server_options_t;
struct rtu_server_options_t
{
	int		fd;
	u32		seed;
	u32		frame_gap_ms;	/* explicit inter-frame silence override; 0 = auto */
	u32		baud;		/* serial baud rate for RTU timing; 0 = unknown */
	device_t**	devices;
	u32		device_count;
};

/* Name: rtu_t35_us
 * Description: Modbus RTU t3.5 inter-frame gap in microseconds for `baud`.
 * Returns 0 when baud is unknown/0. For baud > 19200 Modbus specifies fixed
 * 1750 us; otherwise t3.5 = ceil(3.5 * 11 * 1_000_000 / baud).
 * */
u32	rtu_t35_us(u32 baud);

/* Name: rtu_t15_us
 * Description: Modbus RTU t1.5 inter-character gap in microseconds for `baud`.
 * Returns 0 when baud is unknown/0. For baud > 19200 Modbus specifies fixed
 * 750 us; otherwise t1.5 = ceil(1.5 * 11 * 1_000_000 / baud).
 * */
u32	rtu_t15_us(u32 baud);

/* Name: rtu_effective_gap_ms
 * Description: resolves the inter-frame gap used by the RTU server.
 * frame_gap_ms > 0 wins (explicit override). Otherwise baud-derived t3.5 is
 * used (rounded up to whole ms for poll/clock-ms scheduling). If both are
 * unknown, returns RTU_DEFAULT_FRAME_GAP_MS.
 * */
u32	rtu_effective_gap_ms(u32 frame_gap_ms, u32 baud);

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
