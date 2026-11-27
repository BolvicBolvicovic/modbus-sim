/**
 * @file
 * @brief POSIX os_mutex_t on top of a recursive pthread mutex.
 */
#include "os/os_mutex.h"
#include <pthread.h>

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(os_mutex_t), "os_mutex_t too small");
_Static_assert(_Alignof(pthread_mutex_t) <= _Alignof(os_mutex_t), "os_mutex_t misaligned");

bool
os_mutex_init(os_mutex_t* mutex)
{
	pthread_mutexattr_t	attr;
	int			rc;

	if (pthread_mutexattr_init(&attr) != 0)
		return false;
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	rc = pthread_mutex_init((pthread_mutex_t*)mutex, &attr);
	pthread_mutexattr_destroy(&attr);
	return rc == 0;
}

void
os_mutex_destroy(os_mutex_t* mutex)
{
	pthread_mutex_destroy((pthread_mutex_t*)mutex);
}

void
os_mutex_lock(os_mutex_t* mutex)
{
	pthread_mutex_lock((pthread_mutex_t*)mutex);
}

void
os_mutex_unlock(os_mutex_t* mutex)
{
	pthread_mutex_unlock((pthread_mutex_t*)mutex);
}
