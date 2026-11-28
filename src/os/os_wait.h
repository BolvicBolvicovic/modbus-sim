/**
 * @file
 * @brief Event wait over sockets; the only place poll() lives.
 *
 * os_wait_entry_t is the platform's own poll entry (struct pollfd, WSAPOLLFD), so the table the
 * callers fill goes to poll()/WSAPoll() untouched. This header includes the OS header for that
 * reason; check_deps.sh looks at direct includes only and net/ never includes the OS header itself.
 * The Windows build waits on sockets only (serial/PTY/fd modes are a stub that never opens a handle);
 * a COM-port wait will need its own wrapper.
 */
#ifndef OS_WAIT_H
#define OS_WAIT_H

#include "os/os_socket.h"
#include "vlib/c_types.h"

#ifdef _WIN32
#include <winsock2.h>
typedef WSAPOLLFD os_wait_entry_t;
#else
#include <poll.h>
typedef struct pollfd os_wait_entry_t;
#endif

/** @brief Event bits of os_wait_entry_t.events and .revents (the platform's own values). */
enum
{
#ifdef _WIN32
	OS_WAIT_IN    = POLLRDNORM,
	OS_WAIT_OUT   = POLLWRNORM,
#else
	OS_WAIT_IN    = POLLIN,
	OS_WAIT_OUT   = POLLOUT,
#endif
	OS_WAIT_ERR   = POLLERR,
	OS_WAIT_HUP   = POLLHUP,
	OS_WAIT_INVAL = POLLNVAL,
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
