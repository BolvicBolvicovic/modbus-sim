/**
 * @file
 * @brief Event wait over sockets and byte-stream (serial/fd) handles; the only place poll() lives.
 *
 * Every entry carries its kind so a Windows implementation can wait on sockets (WSAPoll) and
 * on serial handles (overlapped I/O plus WaitForMultipleObjects) differently; the Windows
 * build waits on sockets only and flags serial entries as invalid. The table is a plain
 * fixed-size caller-owned array.
 */
#ifndef OS_WAIT_H
#define OS_WAIT_H

#include "os/os_socket.h"
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
	OS_WAIT_MAX   = 512,	/**< most entries one os_wait() call takes (8 servers of 38 entries each fit) */
};

/** @brief One handle to wait on: events is the request, revents the result. */
struct os_wait_entry_t
{
	os_socket_t	fd;		/**< socket or serial handle; OS_SOCKET_INVALID (-1) entries are ignored */
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
