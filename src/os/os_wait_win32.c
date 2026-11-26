/**
 * @file
 * @brief Windows os_wait() on top of WSAPoll (sockets only).
 */
#include "os/os_wait.h"
#include "os/os_win32.h"
#include <winsock2.h>
#include <windows.h>

/** @brief Longest single WSAPoll wait, so a stop request is seen quickly. */
enum
{
	WAIT_SLICE_MS = 50,
};

int
os_wait(os_wait_entry_t* entries, u32 count, int timeout_ms)
{
	WSAPOLLFD	fds[OS_WAIT_MAX];
	u32		owner[OS_WAIT_MAX];
	u32		n        = 0;
	int		rc       = 0;
	DWORD		deadline = GetTickCount() + (DWORD)timeout_ms;

	if (count > OS_WAIT_MAX)
		count = OS_WAIT_MAX;
	for (u32 i = 0; i < count; i++)
	{
		entries[i].revents = 0;
		if (entries[i].kind == OS_WAIT_SERIAL)
		{
			/* Serial handles are not implemented on Windows: report the entry as invalid. */
			entries[i].revents = OS_WAIT_INVAL;
			rc++;
			continue;
		}
		if (entries[i].fd == OS_SOCKET_INVALID)
			continue;
		fds[n].fd      = entries[i].fd;
		fds[n].events  = (SHORT)(((entries[i].events & OS_WAIT_IN) ? POLLRDNORM : 0) |
					 ((entries[i].events & OS_WAIT_OUT) ? POLLWRNORM : 0));
		fds[n].revents = 0;
		owner[n++]     = i;
	}
	if (rc > 0)
		return rc;
	for (;;)
	{
		int slice = WAIT_SLICE_MS;

		if (InterlockedExchange(&os_win32_interrupt, 0))
		{
			WSASetLastError(WSAEINTR);
			return -1;
		}
		if (timeout_ms >= 0)
		{
			int left = (int)(deadline - GetTickCount());

			if (left < 0)
				left = 0;
			if (left < slice)
				slice = left;
		}
		/* WSAPoll rejects an empty table: sleep out the slice instead. */
		if (n == 0)
		{
			Sleep((DWORD)slice);
			rc = 0;
		}
		else
		{
			rc = WSAPoll(fds, n, slice);
			if (rc < 0)
				return -1;
		}
		if (rc > 0 ||
		    (timeout_ms >= 0 && (int)(deadline - GetTickCount()) <= 0))
			break;
	}
	for (u32 i = 0; i < n; i++)
		entries[owner[i]].revents =
			(u32)(((fds[i].revents & POLLRDNORM) ? OS_WAIT_IN : 0) |
			      ((fds[i].revents & POLLWRNORM) ? OS_WAIT_OUT : 0) |
			      ((fds[i].revents & POLLERR) ? OS_WAIT_ERR : 0) |
			      ((fds[i].revents & POLLHUP) ? OS_WAIT_HUP : 0) |
			      ((fds[i].revents & POLLNVAL) ? OS_WAIT_INVAL : 0));
	return rc;
}
