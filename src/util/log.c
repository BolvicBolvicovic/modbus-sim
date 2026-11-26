/**
 * @file
 * @brief Process-wide logging with a level, a format and a pluggable sink.
 */
#include "util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static log_level_t	g_level    = LOG_LEVEL_INFO;
static log_format_t	g_format   = LOG_FORMAT_TEXT;
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

void
log_set_format(log_format_t format)
{
	g_format = format;
}

log_format_t
log_format(void)
{
	return g_format;
}

bool
log_enabled(log_level_t level)
{
	return level < LOG_LEVEL_OFF && level >= g_level;
}

u32
log_json_escape(char* out, u32 size, const char* text)
{
	u32 used = 0;

	if (size == 0)
		return 0;
	for (const unsigned char* p = (const unsigned char*)text; *p; p++)
	{
		char	esc[7];
		u32	n;

		if (*p == '"' || *p == '\\')
			n = (u32)snprintf(esc, sizeof(esc), "\\%c", *p);
		else if (*p < 0x20)
			n = (u32)snprintf(esc, sizeof(esc), "\\u%04x", *p);
		else
		{
			esc[0] = (char)*p;
			esc[1] = '\0';
			n = 1;
		}
		if (used + n >= size)
			break;
		memcpy(out + used, esc, n);
		used += n;
	}
	out[used] = '\0';
	return used;
}

static void __attribute__((format(printf, 3, 0)))
log_vwrite(log_level_t level, const char* fields, const char* fmt, va_list ap)
{
	char		message[1024];
	char		line[3072];
	char		stamp[32] = "";
	struct timespec	ts;
	struct tm	tm;

	vsnprintf(message, sizeof(message), fmt, ap);
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
	    gmtime_r(&ts.tv_sec, &tm))
		snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);

	if (g_format == LOG_FORMAT_JSON)
	{
		char	msg[2048];
		int	n;

		log_json_escape(msg, sizeof(msg), message);
		n = snprintf(line, sizeof(line), "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\"%s%s}",
			     stamp, LEVEL_NAMES[level], msg, fields ? "," : "", fields ? fields : "");
		if (n < 0 || (size_t)n >= sizeof(line))
			snprintf(line, sizeof(line), "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"(line too long)\"}",
				 stamp, LEVEL_NAMES[level]);
	}
	else
		snprintf(line, sizeof(line), "%s", message);

	if (g_sink)
	{
		g_sink(g_sink_ctx, level, line);
		return;
	}
	if (g_format == LOG_FORMAT_JSON)
		fprintf(stderr, "%s\n", line);
	else if (stamp[0])
		fprintf(stderr, "%s [%s] %s\n", stamp, LEVEL_NAMES[level], line);
	else
		fprintf(stderr, "[%s] %s\n", LEVEL_NAMES[level], line);
}

void
log_write(log_level_t level, const char* fmt, ...)
{
	va_list ap;

	if (!log_enabled(level))
		return;
	va_start(ap, fmt);
	log_vwrite(level, NULL, fmt, ap);
	va_end(ap);
}

void
log_write_fields(log_level_t level, const char* fields, const char* fmt, ...)
{
	va_list ap;

	if (!log_enabled(level))
		return;
	va_start(ap, fmt);
	log_vwrite(level, fields, fmt, ap);
	va_end(ap);
}
