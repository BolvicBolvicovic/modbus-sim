/**
 * @file
 * @brief tty line configuration for serial-like byte streams.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include "vlib/c_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

/** @brief Serial parity for byte streams; mark/space need CMSPAR on the host. */
typedef enum parity_t parity_t;
enum parity_t
{
	PARITY_NONE,
	PARITY_EVEN,
	PARITY_ODD,
	PARITY_MARK,
	PARITY_SPACE,
};

/**
 * @brief Line-format c_cflag bits to clear and set for the requested serial mode. data_bits is
 *        7 or 8.
 *
 * Returns false when mark/space parity is requested without CMSPAR support.
 */
bool		serial_line_flags(u32 data_bits, parity_t parity, u32 stop_bits,
				  tcflag_t* clear, tcflag_t* set);

/** @brief Configured baud of a real tty, or 0 for PTYs and non-tty fds. */
u32		serial_tty_baud(int fd);

/**
 * @brief Opens a PTY (pty = true) or an existing device at path, sets raw mode with the
 *        selected data bits, parity and stop bits, and returns the fd to serve.
 *
 * For a PTY, slave_fd receives the slave fd to keep open and name receives its path; both are
 * cleared for device mode. Returns -1 on error, closing any fds already opened.
 */
int		open_serial_fd(bool pty, const char* label, const char* path, u32 baud,
			       u32 data_bits, parity_t parity, u32 stop_bits,
			       int* slave_fd, char* name, size_t name_size);

#endif
