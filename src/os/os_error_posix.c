/**
 * @file
 * @brief POSIX last-error classification (errno).
 */
#include "os/os_error.h"
#include <errno.h>
#include <string.h>

int
os_last_error(void)
{
	return errno;
}

const char*
os_error_text(int code)
{
	return strerror(code);
}

const char*
os_last_error_text(void)
{
	return strerror(errno);
}

bool
os_would_block(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool
os_interrupted(void)
{
	return errno == EINTR;
}

bool
os_retry(void)
{
	return os_would_block() || os_interrupted();
}

bool
os_out_of_handles(void)
{
	return errno == EMFILE || errno == ENFILE;
}

bool
os_conn_aborted(void)
{
	return errno == ECONNABORTED;
}

bool
os_conn_refused(void)
{
	return errno == ECONNREFUSED;
}
