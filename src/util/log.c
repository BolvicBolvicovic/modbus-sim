#include "util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static log_level_t	g_level    = LOG_LEVEL_INFO;
static log_sink_t	g_sink     = NULL;
static void*		g_sink_ctx = NULL;

static const char* const LEVEL_NAMES[] = {
	[LOG_LEVEL_DEBUG] = "debug",
	[LOG_LEVEL_INFO]  = "info",
	[LOG_LEVEL_WARN]  = "warn",
	[LOG_LEVEL_OFF]   = "off",
};

void
log_set_sink(log_sink_t sink, void* ctx)
{
	g_sink     = sink;
	g_sink_ctx = ctx;
}

void
log_set_level(log_level_t level)
{
	g_level = level;
}

bool
log_enabled(log_level_t level)
{
	return level < LOG_LEVEL_OFF && level >= g_level;
}

void
log_write(log_level_t level, const char* fmt, ...)
{
	char		message[1024];
	va_list		ap;
	struct timespec	ts;
	struct tm	tm;

	if (!log_enabled(level))
		return;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	if (g_sink)
	{
		g_sink(g_sink_ctx, level, message);
		return;
	}
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
	    gmtime_r(&ts.tv_sec, &tm))
		fprintf(stderr, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ [%s] %s\n",
		    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		    tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
		    LEVEL_NAMES[level], message);
	else
		fprintf(stderr, "[%s] %s\n", LEVEL_NAMES[level], message);
}
