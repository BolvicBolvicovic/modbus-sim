/**
 * @file
 * @brief POSIX os_wait() on top of poll().
 */
#include "os/os_wait.h"

int
os_wait(os_wait_entry_t* entries, u32 count, int timeout_ms)
{
	return poll(entries, count, timeout_ms);
}
