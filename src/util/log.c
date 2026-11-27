/**
 * @file
 * @brief Process-wide logging with a level, a format and a pluggable sink.
 */
#include "util/log.h"
#include "os/os_time.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static log_level_t	g_level    = LOG_LEVEL_INFO;
static log_format_t	g_format   = LOG_FORMAT_TEXT;
static log_sink_t	g_sink     = NULL;
static void*		g_sink_ctx = NULL;
static const char*	g_prefix   = NULL;

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
log_set_prefix(const char* prefix)
{
	g_prefix = prefix && prefix[0] ? prefix : NULL;
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
	os_utc_t	utc;

	int		head = g_prefix ? snprintf(message, sizeof(message), "[%s] ", g_prefix) : 0;

	if (head < 0 || (size_t)head >= sizeof(message))
		head = 0;
	vsnprintf(message + head, sizeof(message) - (size_t)head, fmt, ap);
	if (os_utc_now(&utc))
		snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
			 utc.year, utc.month, utc.day, utc.hour, utc.min, utc.sec, utc.msec);

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
