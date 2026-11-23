#ifndef SERIAL_H
#define SERIAL_H

#include "vlib/c_types.h"
#include <stdbool.h>
#include <termios.h>

/* Serial parity for RTU byte streams; mark/space need CMSPAR on the host. */
typedef enum parity_t parity_t;
enum parity_t
{
	PARITY_NONE,
	PARITY_EVEN,
	PARITY_ODD,
	PARITY_MARK,
	PARITY_SPACE,
};

/* Line-format c_cflag bits to clear and set for the requested serial mode.
 * Returns false when mark/space parity is requested without CMSPAR support. */
bool		serial_line_flags(parity_t parity, u32 stop_bits, tcflag_t* clear, tcflag_t* set);

#endif
