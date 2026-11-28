/**
 * @file
 * @brief POSIX os_wait() on top of poll().
 */
#include "os/os_wait.h"
#include <poll.h>
#include <stddef.h>

_Static_assert(sizeof(os_wait_entry_t) == sizeof(struct pollfd) &&
	       offsetof(os_wait_entry_t, events) == offsetof(struct pollfd, events) &&
	       offsetof(os_wait_entry_t, revents) == offsetof(struct pollfd, revents),
	       "os_wait_entry_t must match struct pollfd");
_Static_assert(OS_WAIT_IN == POLLIN &&
	       OS_WAIT_OUT == POLLOUT &&
	       OS_WAIT_ERR == POLLERR &&
	       OS_WAIT_HUP == POLLHUP &&
	       OS_WAIT_INVAL == POLLNVAL,
	       "OS_WAIT_* must be the poll() bits");

int
os_wait(os_wait_entry_t* entries, u32 count, int timeout_ms)
{
	return poll((struct pollfd*)entries, count, timeout_ms);
}
