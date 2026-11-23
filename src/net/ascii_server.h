#ifndef ASCII_SERVER_H
#define ASCII_SERVER_H

#include "vlib/c_types.h"

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

/* Name: ascii_server_run
 * Description: serves Modbus ASCII on options->fd until SIGINT or SIGTERM, or
 * until the peer closes the fd. Single thread, poll-based like the RTU server.
 * Returns 0 after a clean stop, non-zero on startup/runtime error.
 * */
s32	ascii_server_run(const ascii_server_options_t* options);

#endif
