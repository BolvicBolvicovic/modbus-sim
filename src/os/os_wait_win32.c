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
	int	rc       = 0;
	DWORD	deadline = GetTickCount() + (DWORD)timeout_ms;

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
		if (count == 0)
			Sleep((DWORD)slice);
		else if (WSAPoll(entries, count, slice) < 0)
			return -1;
		/* WSAPoll reports INVALID_SOCKET entries as POLLNVAL; poll() ignores them. */
		rc = 0;
		for (u32 i = 0; i < count; i++)
		{
			if (entries[i].fd == OS_SOCKET_INVALID)
				entries[i].revents = 0;
			if (entries[i].revents)
				rc++;
		}
		if (rc > 0 ||
		    (timeout_ms >= 0 && (int)(deadline - GetTickCount()) <= 0))
			return rc;
	}
}
