/**
 * @file
 * @brief Standard termios baud rates.
 */
#ifndef BAUD_H
#define BAUD_H

#include "vlib/c_types.h"
#include <stdbool.h>
#include <termios.h>

/** @brief Standard termios baud rates supported by mogbus RTU timing/tty config. */
bool	baud_is_standard(u32 baud);
bool	baud_to_speed(u32 baud, speed_t* out);
u32	baud_from_speed(speed_t speed);

#endif
