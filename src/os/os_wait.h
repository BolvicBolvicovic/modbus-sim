/**
 * @file
 * @brief Event wait over sockets; the only place poll() lives.
 *
 * os_wait_entry_t has the exact layout of the platform's poll entry (struct pollfd, WSAPOLLFD)
 * and the OS_WAIT_* bits are the platform's values, so the table callers fill goes to
 * poll()/WSAPoll() untouched. The OS headers stay out of this header (winsock2.h would leak
 * windows.h macros into every includer); os_wait_posix.c and os_wait_win32.c assert that layout
 * and values match. The Windows build waits on sockets only (serial/PTY/fd modes are a stub that
 * never opens a handle); a COM-port wait will need its own wrapper.
 */
#ifndef OS_WAIT_H
#define OS_WAIT_H

#include "os/os_socket.h"
#include "vlib/c_types.h"

typedef struct os_wait_entry_t os_wait_entry_t;

/** @brief One handle to wait on, laid out as the platform's poll entry. */
struct os_wait_entry_t
{
	os_socket_t	fd;		/**< OS_SOCKET_INVALID entries are ignored */
	short		events;		/**< requested OS_WAIT_* bits */
	short		revents;	/**< reported OS_WAIT_* bits */
};

/** @brief Event bits of os_wait_entry_t.events and .revents (the platform's own poll values). */
enum
{
#ifdef _WIN32
	OS_WAIT_IN    = 0x0100,	/* POLLRDNORM */
	OS_WAIT_OUT   = 0x0010,	/* POLLWRNORM */
	OS_WAIT_ERR   = 0x0001,
	OS_WAIT_HUP   = 0x0002,
	OS_WAIT_INVAL = 0x0004,
#else
	OS_WAIT_IN    = 0x0001,
	OS_WAIT_OUT   = 0x0004,
	OS_WAIT_ERR   = 0x0008,
	OS_WAIT_HUP   = 0x0010,
	OS_WAIT_INVAL = 0x0020,
#endif
};

/**
 * @brief Waits until an entry is ready or timeout_ms elapses (negative waits forever).
 *
 * Fills revents; entries with fd OS_SOCKET_INVALID are ignored (revents 0). Returns the number
 * of ready entries, 0 on timeout, or -1 on error (see os_interrupted()). ERR, HUP and INVAL are
 * reported in revents whether or not they were requested.
 */
int	os_wait(os_wait_entry_t* entries, u32 count, int timeout_ms);

#endif
