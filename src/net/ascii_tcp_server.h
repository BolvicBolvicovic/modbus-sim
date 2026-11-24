#ifndef ASCII_TCP_SERVER_H
#define ASCII_TCP_SERVER_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct device_t device_t;

/* Name: ascii_tcp_server_options_t
 * Description: Modbus ASCII-over-TCP server options. Devices are borrowed, not copied.
 * Raw ASCII frames (':' + hex [addr][PDU][LRC] + CR LF) are carried over TCP
 * byte streams; this is not MBAP framing. No serial line options apply. */
typedef struct ascii_tcp_server_options_t ascii_tcp_server_options_t;
struct ascii_tcp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	u32		idle_timeout_ms;
	device_t**	devices;
	u32		device_count;
};

/* Name: ascii_tcp_server_run
 * Description: serves Modbus ASCII frames over TCP until SIGINT or SIGTERM.
 * Single thread, poll-based. Returns 0 after a clean stop, non-zero on startup error.
 * */
s32	ascii_tcp_server_run(const ascii_tcp_server_options_t* options);

#endif
