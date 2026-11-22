#ifndef BAUD_H
#define BAUD_H

#include "vlib/c_types.h"
#include <stdbool.h>
#include <termios.h>

/* Standard termios baud rates supported by modbus-sim RTU timing/tty config. */
bool	baud_is_standard(u32 baud);
bool	baud_to_speed(u32 baud, speed_t* out);
u32	baud_from_speed(speed_t speed);

#endif
