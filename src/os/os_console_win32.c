/**
 * @file
 * @brief Windows stop handling with SetConsoleCtrlHandler.
 */
#include "os/os_console.h"
#include "os/os_win32.h"
#include <windows.h>
#include <stddef.h>

volatile long os_win32_interrupt;

static void (*g_handler)(void);

/* Runs on a thread the system creates for the event; the handler only sets flags. */
static BOOL WINAPI
on_event(DWORD event)
{
	if (event != CTRL_C_EVENT &&
	    event != CTRL_BREAK_EVENT &&
	    event != CTRL_CLOSE_EVENT &&
	    event != CTRL_LOGOFF_EVENT &&
	    event != CTRL_SHUTDOWN_EVENT)
		return FALSE;
	InterlockedExchange(&os_win32_interrupt, 1);
	if (g_handler)
		g_handler();
	return TRUE;
}

void
os_console_on_stop(void (*handler)(void))
{
	g_handler = handler;
	SetConsoleCtrlHandler(on_event, TRUE);
}
