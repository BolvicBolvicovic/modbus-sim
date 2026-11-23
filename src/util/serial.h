#ifndef SERIAL_H
#define SERIAL_H

#include "vlib/c_types.h"
#include <stdbool.h>
#include <stddef.h>
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

/* Configured baud of a real tty, or 0 for PTYs and non-tty fds. */
u32		serial_tty_baud(int fd);

/* Opens a PTY (pty = true) or an existing device at path, sets raw mode and
 * non-blocking I/O, and returns the fd to serve. For a PTY, slave_fd receives
 * the slave fd to keep open and name receives its path; both are cleared for
 * device mode. Returns -1 on error, closing any fds already opened. */
int		open_serial_fd(bool pty, const char* label, const char* path, u32 baud,
			       parity_t parity, u32 stop_bits, int* slave_fd,
			       char* name, size_t name_size);

#endif
