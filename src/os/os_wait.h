/**
 * @file
 * @brief Event wait over sockets and byte-stream (serial/fd) handles; the only place poll() lives.
 *
 * A Windows implementation fits this shape: WSAPoll for the socket entries and overlapped I/O
 * plus WaitForMultipleObjects (comm events) for serial entries, which is why every entry
 * carries its kind and why the table is a plain fixed-size caller-owned array.
 */
#ifndef OS_WAIT_H
#define OS_WAIT_H

#include "vlib/c_types.h"

typedef enum os_wait_kind_t os_wait_kind_t;
typedef struct os_wait_entry_t os_wait_entry_t;

/** @brief What a wait entry refers to. */
enum os_wait_kind_t
{
	OS_WAIT_SOCKET,
	OS_WAIT_SERIAL,	/**< serial device, PTY or host-owned byte-stream fd */
};

/** @brief Event bits of os_wait_entry_t.events and .revents. */
enum
{
	OS_WAIT_IN    = 1,
	OS_WAIT_OUT   = 2,
	OS_WAIT_ERR   = 4,
	OS_WAIT_HUP   = 8,
	OS_WAIT_INVAL = 16,
	OS_WAIT_MAX   = 64,	/**< most entries one os_wait() call takes */
};

/** @brief One handle to wait on: events is the request, revents the result. */
struct os_wait_entry_t
{
	int		fd;		/**< socket or serial handle; negative entries are ignored */
	os_wait_kind_t	kind;
	u32		events;
	u32		revents;
};

/**
 * @brief Waits until an entry is ready or timeout_ms elapses (negative waits forever).
 *
 * Returns the number of ready entries, 0 on timeout, or -1 on error (see os_interrupted()).
 * ERR, HUP and INVAL are reported in revents whether or not they were requested.
 */
int	os_wait(os_wait_entry_t* entries, u32 count, int timeout_ms);

#endif
