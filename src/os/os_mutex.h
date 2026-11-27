/**
 * @file
 * @brief Recursive mutex with caller-owned storage, so a context can embed its lock and the
 *        library still never allocates.
 */
#ifndef OS_MUTEX_H
#define OS_MUTEX_H

#include <stdalign.h>
#include <stdbool.h>

typedef struct os_mutex_t os_mutex_t;

/** @brief A recursive mutex. The storage is opaque and sized for pthread_mutex_t and CRITICAL_SECTION. */
struct os_mutex_t
{
	alignas(16) unsigned char	opaque[80];
};

/** @brief Initialises a recursive mutex in place; false if the OS refuses. */
bool	os_mutex_init(os_mutex_t* mutex);

/** @brief Destroys an unlocked mutex. */
void	os_mutex_destroy(os_mutex_t* mutex);

/** @brief Locks; the owning thread may lock again and must unlock as many times. */
void	os_mutex_lock(os_mutex_t* mutex);

/** @brief Releases one level of the lock. */
void	os_mutex_unlock(os_mutex_t* mutex);

#endif
