/**
 * @file
 * @brief Windows-only state shared by the os_*_win32.c files.
 */
#ifndef OS_WIN32_H
#define OS_WIN32_H

/**
 * @brief Set by the console control handler (another thread) and consumed by os_wait(), which
 *        then fails with WSAEINTR like a POSIX wait interrupted by a signal.
 */
extern volatile long os_win32_interrupt;

#endif
