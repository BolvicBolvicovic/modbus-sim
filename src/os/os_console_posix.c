/**
 * @file
 * @brief POSIX stop handling with sigaction.
 */
#include "os/os_console.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void (*g_handler)(void);

static void
on_signal(int signo)
{
	(void)signo;
	if (g_handler)
		g_handler();
}

void
os_console_on_stop(void (*handler)(void))
{
	struct sigaction sa = { 0 };

	g_handler     = handler;
	sa.sa_handler = on_signal;	/* no SA_RESTART: a blocked wait must wake up */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

bool
os_console_terminal(u32* rows, u32* cols)
{
	struct winsize	size = { 0 };
	const char*	term = getenv("TERM");

	if (!isatty(STDOUT_FILENO) ||
	    (term && strcmp(term, "dumb") == 0))
		return false;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0)
		size.ws_row = size.ws_col = 0;
	if (rows)
		*rows = size.ws_row ? size.ws_row : 24;
	if (cols)
		*cols = size.ws_col ? size.ws_col : 80;
	return true;
}
