/**
 * @file
 * @brief POSIX os_wait() on top of poll().
 */
#include "os/os_wait.h"
#include <poll.h>

int
os_wait(os_wait_entry_t* entries, u32 count, int timeout_ms)
{
	struct pollfd	fds[OS_WAIT_MAX];
	int		rc;

	if (count > OS_WAIT_MAX)
		count = OS_WAIT_MAX;
	for (u32 i = 0; i < count; i++)
	{
		fds[i].fd      = entries[i].fd;
		fds[i].events  = (short)(((entries[i].events & OS_WAIT_IN) ? POLLIN : 0) |
					 ((entries[i].events & OS_WAIT_OUT) ? POLLOUT : 0));
		fds[i].revents = 0;
	}
	rc = poll(fds, count, timeout_ms);
	for (u32 i = 0; i < count; i++)
		entries[i].revents = rc <= 0 ? 0 :
			(u32)(((fds[i].revents & POLLIN) ? OS_WAIT_IN : 0) |
			      ((fds[i].revents & POLLOUT) ? OS_WAIT_OUT : 0) |
			      ((fds[i].revents & POLLERR) ? OS_WAIT_ERR : 0) |
			      ((fds[i].revents & POLLHUP) ? OS_WAIT_HUP : 0) |
			      ((fds[i].revents & POLLNVAL) ? OS_WAIT_INVAL : 0));
	return rc;
}
