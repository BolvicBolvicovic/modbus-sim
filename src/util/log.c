#include "util/log.h"
#include <stdarg.h>
#include <stdio.h>

static bool	g_verbose = false;

void
log_set_verbose(bool verbose)
{
	g_verbose = verbose;
}

bool
log_is_verbose(void)
{
	return g_verbose;
}

void
log_write(const char* level, const char* fmt, ...)
{
	va_list	ap;

	fprintf(stderr, "[%s] ", level);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
