#ifndef SERVER_H
#define SERVER_H

#include "vlib/c_types.h"
#include "device/device.h"

/* Name: server_options_t
 * Description: what the server needs to run. Devices are borrowed, not copied.
 * */
typedef struct server_options_t server_options_t;
struct server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	device_t**	devices;
	u32		device_count;
};

/* Name: server_run
 * Description: serves Modbus TCP until SIGINT or SIGTERM. Single thread, no blocking sleeps:
 * a delayed response never stalls other clients.
 * Returns 0 after a clean stop, non-zero if the server could not start.
 * */
s32	server_run(const server_options_t* options);

#endif
