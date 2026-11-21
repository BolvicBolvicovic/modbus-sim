#include "util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static bool	g_verbose = false;

void
log_set_verbose(bool verbose)
{
	g_verbose = verbose;
}

bool
log_verbose(void)
{
	return g_verbose;
}

void
log_write(const char* level, const char* fmt, ...)
{
	va_list		ap;
	struct timespec	ts;
	struct tm	tm;

	if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
	    gmtime_r(&ts.tv_sec, &tm))
		fprintf(stderr, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ [%s] ",
		    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		    tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L, level);
	else
		fprintf(stderr, "[%s] ", level);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
