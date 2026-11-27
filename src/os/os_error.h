/**
 * @file
 * @brief Classification of the last OS error of the calling thread, so no caller reads errno
 *        (or WSAGetLastError()) itself.
 */
#ifndef OS_ERROR_H
#define OS_ERROR_H

#include <stdbool.h>

/**
 * @brief Raw error code of the last failed OS call of the calling thread.
 *
 * errno on POSIX, WSAGetLastError() on Windows; thread-local, and only meaningful right after
 * a call that failed. Pass it to os_error_text().
 */
int		os_last_error(void);

/**
 * @brief Human-readable text for an error code from os_last_error() or os_connect_status().
 *
 * The returned string is owned by the OS layer (strerror() result, or a per-thread buffer on
 * Windows): do not free or modify it, and copy it before the next call on the same thread.
 */
const char*	os_error_text(int code);

/** @brief Text of the calling thread's last error; same as os_error_text(os_last_error()). */
const char*	os_last_error_text(void);

/** @brief True when the last failed call would have blocked on a nonblocking handle (EAGAIN, WSAEWOULDBLOCK). */
bool		os_would_block(void);

/** @brief True when the last failed call was interrupted by a signal or console event (EINTR, WSAEINTR). */
bool		os_interrupted(void);

/** @brief True when the last failed call may simply be retried later: os_would_block() or os_interrupted(). */
bool		os_retry(void);

/** @brief True when accept or open failed for lack of descriptors (EMFILE/ENFILE, WSAEMFILE/WSAENOBUFS). */
bool		os_out_of_handles(void);

/** @brief True when a pending connection was aborted before accept() returned it (ECONNABORTED); the listener is still usable. */
bool		os_conn_aborted(void);

/** @brief True when the last connect failed because nothing listens at the target (ECONNREFUSED). */
bool		os_conn_refused(void);

#endif
