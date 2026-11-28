/**
 * @file
 * @brief Process-level console events: the stop request (Ctrl-C, termination) and the flag
 *        type a handler or another thread uses to stop a server loop.
 */
#ifndef OS_CONSOLE_H
#define OS_CONSOLE_H

#include "vlib/c_types.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>

/** @brief Flag that is safe to set from a signal handler or another thread. */
typedef _Atomic sig_atomic_t os_stop_flag_t;

/**
 * @brief Calls handler on SIGINT and SIGTERM (without SA_RESTART, so a blocked wait returns)
 *        and ignores SIGPIPE.
 *
 * The handler runs in signal context: it may only do async-signal-safe work.
 *
 * On Windows the handler also runs on Ctrl-C, Ctrl-Break, console close, logoff and shutdown,
 * and when another process signals the named event `Local\\mogbus-stop-<pid>` (<pid> is the
 * decimal process id; OpenEvent + SetEvent), which needs no shared console.
 */
void	os_console_on_stop(void (*handler)(void));

/**
 * @brief True when stdout is a terminal that understands ANSI escape sequences; stores its size.
 *
 * On Windows this turns on virtual-terminal processing for the console (Windows 10 or newer).
 * A size of 0 in the console is replaced by 24 rows by 80 columns. TERM=dumb is not a terminal.
 *
 * @param rows Receives the height in lines; may be NULL.
 * @param cols Receives the width in columns; may be NULL.
 */
bool	os_console_terminal(u32* rows, u32* cols);

#endif
