/**
 * @file
 * @brief Windows stop handling with SetConsoleCtrlHandler and a named stop event.
 */
#include "os/os_console.h"
#include "os/os_win32.h"
#include <windows.h>
#include <stddef.h>
#include <stdio.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

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

/* Waits for the named stop event (see os_console_on_stop()); one wake-up is one stop request. */
static DWORD WINAPI
stop_event_thread(LPVOID event)
{
	while (WaitForSingleObject((HANDLE)event, INFINITE) == WAIT_OBJECT_0)
	{
		InterlockedExchange(&os_win32_interrupt, 1);
		if (g_handler)
			g_handler();
	}
	return 0;
}

void
os_console_on_stop(void (*handler)(void))
{
	char	name[48];
	HANDLE	event;

	g_handler = handler;
	SetConsoleCtrlHandler(on_event, TRUE);
	/* Console events reach only processes that share a console; the named event works from any
	 * process of the same session (a test harness, a service wrapper). */
	snprintf(name, sizeof(name), "Local\\mogbus-stop-%lu", (unsigned long)GetCurrentProcessId());
	event = CreateEventA(NULL, FALSE, FALSE, name);
	if (event)
		CloseHandle(CreateThread(NULL, 0, stop_event_thread, event, 0, NULL));
}

bool
os_console_terminal(u32* rows, u32* cols)
{
	HANDLE				out  = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD				mode = 0;
	CONSOLE_SCREEN_BUFFER_INFO	info;

	if (out == INVALID_HANDLE_VALUE ||
	    !GetConsoleMode(out, &mode) ||
	    !SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
	    !GetConsoleScreenBufferInfo(out, &info))
		return false;
	if (rows)
		*rows = (u32)(info.srWindow.Bottom - info.srWindow.Top + 1);
	if (cols)
		*cols = (u32)(info.srWindow.Right - info.srWindow.Left + 1);
	return true;
}
