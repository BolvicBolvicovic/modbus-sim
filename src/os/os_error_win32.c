/**
 * @file
 * @brief Windows last-error classification (WSAGetLastError, which shares the thread's GetLastError slot).
 */
#include "os/os_error.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

int
os_last_error(void)
{
	return WSAGetLastError();
}

const char*
os_error_text(int code)
{
	static _Thread_local char	text[256];
	DWORD				len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
							     FORMAT_MESSAGE_IGNORE_INSERTS,
							     NULL, (DWORD)code, 0, text,
							     (DWORD)sizeof(text), NULL);

	while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n' || text[len - 1] == ' '))
		len--;
	if (len == 0)
		snprintf(text, sizeof(text), "error %d", code);
	else
		text[len] = '\0';
	return text;
}

const char*
os_last_error_text(void)
{
	return os_error_text(WSAGetLastError());
}

bool
os_would_block(void)
{
	return WSAGetLastError() == WSAEWOULDBLOCK;
}

bool
os_interrupted(void)
{
	return WSAGetLastError() == WSAEINTR;
}

bool
os_retry(void)
{
	return os_would_block() || os_interrupted();
}

bool
os_out_of_handles(void)
{
	return WSAGetLastError() == WSAEMFILE || WSAGetLastError() == WSAENOBUFS;
}

bool
os_conn_aborted(void)
{
	return WSAGetLastError() == WSAECONNABORTED;
}

bool
os_conn_refused(void)
{
	return WSAGetLastError() == WSAECONNREFUSED;
}
