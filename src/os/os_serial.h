/**
 * @file
 * @brief Serial devices, PTYs and byte-stream handles: open and configure, raw read/write.
 *
 * A byte-stream handle is also what the `*_fd` server modes hand in, so reads and writes here
 * work on any such handle. Reads and writes are nonblocking; failures leave the error for the
 * os_error.h classifiers.
 */
#ifndef OS_SERIAL_H
#define OS_SERIAL_H

#include "vlib/c_types.h"
#include <stdbool.h>
#include <stddef.h>

/** @brief Serial device, PTY or byte-stream handle. */
typedef int os_serial_t;
typedef enum parity_t parity_t;

/** @brief Invalid serial handle. */
enum
{
	OS_SERIAL_INVALID = -1,
};

/** @brief Serial parity for byte streams; mark/space need CMSPAR on the host. */
enum parity_t
{
	PARITY_NONE,
	PARITY_EVEN,
	PARITY_ODD,
	PARITY_MARK,
	PARITY_SPACE,
};

/** @brief False where serial devices, PTYs and byte-stream handles are not implemented (Windows). */
bool		os_serial_supported(void);

/** @brief True for a standard termios baud rate (9600, 19200, 115200, ...). */
bool		os_baud_is_standard(u32 baud);

/**
 * @brief Line-format c_cflag bits to clear and set for the requested serial mode. data_bits is
 *        7 or 8.
 *
 * Returns false when mark/space parity is requested without CMSPAR support.
 */
bool		os_serial_line_flags(u32 data_bits, parity_t parity, u32 stop_bits,
				     u32* clear, u32* set);

/** @brief Configured baud of a real tty, or 0 for PTYs and non-tty handles. */
u32		os_serial_baud(os_serial_t fd);

/**
 * @brief Opens a serial device (pty = false, `path`) or creates a PTY (pty = true) in raw mode
 *        with the given line format; the handle is nonblocking.
 *
 * baud 0 keeps the device's speed. For a PTY, the slave handle is stored in `slave_fd` (keep it
 * open so the master never sees a hangup) and its name in `name`. Prints the reason to stderr
 * and returns OS_SERIAL_INVALID on failure.
 */
os_serial_t	os_serial_open(bool pty, const char* label, const char* path, u32 baud,
			       u32 data_bits, parity_t parity, u32 stop_bits,
			       os_serial_t* slave_fd, char* name, size_t name_size);

/** @brief Reads up to len bytes: bytes read, 0 at end of stream, -1 on error (see os_retry()). */
s64		os_serial_read(os_serial_t fd, void* buf, u32 len);

/** @brief Writes up to len bytes: bytes written or -1. */
s64		os_serial_write(os_serial_t fd, const void* buf, u32 len);

/** @brief Closes a serial handle; the caller must not use it afterwards. */
void		os_serial_close(os_serial_t fd);

#endif
