/**
 * @file
 * @brief POSIX stop handling with sigaction.
 */
#include "os/os_console.h"
#include <stddef.h>

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
