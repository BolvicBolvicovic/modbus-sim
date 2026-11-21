#ifndef CONFIG_H
#define CONFIG_H

#include "vlib/c_types.h"
#include "vlib/arena.h"
#include "device/device.h"
#include <stdbool.h>

#define CONFIG_MAX_DEVICES		16
#define CONFIG_DEFAULT_PORT		1502
#define CONFIG_DEFAULT_IDLE_TIMEOUT_MS	0
#define CONFIG_DEFAULT_FRAME_GAP_MS	20
#define CONFIG_BIND_MAX			64

/* Name: config_t
 * Description: everything read from a config file.
 * Devices live in the arena given to config_load.
 * */
typedef struct config_t config_t;
struct config_t
{
	char		bind_address[CONFIG_BIND_MAX];
	u16		port;
	u32		seed;			/* seed for fault dice; 0 = fixed default */
	u32		idle_timeout_ms;	/* close idle connections after N ms; 0 = disabled */
	u32		frame_gap_ms;		/* RTU inter-frame silence; 0 means use default */
	u32		device_count;
	device_t*	devices[CONFIG_MAX_DEVICES];
};

/* Name: config_load
 * Description: reads a Scheme config file.
 * - persistent: arena that keeps the devices (must outlive the config)
 * - scratch   : arena for temporary data; may be cleared afterwards
 * On failure returns false and fills `err` with a message a human can act on.
 * */
bool	config_load(arena_t* persistent, arena_t* scratch, const char* path,
		    config_t* out, char* err, u32 err_size);

#endif
