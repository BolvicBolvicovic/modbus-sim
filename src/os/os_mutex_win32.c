/**
 * @file
 * @brief Windows os_mutex_t on top of a CRITICAL_SECTION, which is recursive.
 */
#include "os/os_mutex.h"
#include <windows.h>

_Static_assert(sizeof(CRITICAL_SECTION) <= sizeof(os_mutex_t), "os_mutex_t too small");
_Static_assert(_Alignof(CRITICAL_SECTION) <= _Alignof(os_mutex_t), "os_mutex_t misaligned");

bool
os_mutex_init(os_mutex_t* mutex)
{
	InitializeCriticalSection((CRITICAL_SECTION*)mutex);
	return true;
}

void
os_mutex_destroy(os_mutex_t* mutex)
{
	DeleteCriticalSection((CRITICAL_SECTION*)mutex);
}

void
os_mutex_lock(os_mutex_t* mutex)
{
	EnterCriticalSection((CRITICAL_SECTION*)mutex);
}

void
os_mutex_unlock(os_mutex_t* mutex)
{
	LeaveCriticalSection((CRITICAL_SECTION*)mutex);
}
