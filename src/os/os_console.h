/**
 * @file
 * @brief Process-level console events: the stop request (Ctrl-C, termination) and the flag
 *        type a handler or another thread uses to stop a server loop.
 */
#ifndef OS_CONSOLE_H
#define OS_CONSOLE_H

#include <signal.h>

/** @brief Flag that is safe to set from a signal handler or another thread. */
typedef volatile sig_atomic_t os_stop_flag_t;

/**
 * @brief Calls handler on SIGINT and SIGTERM (without SA_RESTART, so a blocked wait returns)
 *        and ignores SIGPIPE.
 *
 * The handler runs in signal context: it may only do async-signal-safe work.
 */
void	os_console_on_stop(void (*handler)(void));

#endif
