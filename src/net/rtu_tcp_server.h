#ifndef RTU_TCP_SERVER_H
#define RTU_TCP_SERVER_H

#include "vlib/c_types.h"
#include <stdbool.h>

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

/* Name: rtu_tcp_server_run
 * Description: serves Modbus RTU frames over TCP until SIGINT or SIGTERM.
 * Single thread, poll-based. Returns 0 after a clean stop, non-zero on startup error.
 * */
s32	rtu_tcp_server_run(const rtu_tcp_server_options_t* options);

#endif
