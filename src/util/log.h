/**
 * @file
 * @brief Process-wide logging with a level, a format and a pluggable sink.
 */
#ifndef LOG_H
#define LOG_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef enum log_level_t log_level_t;
typedef enum log_format_t log_format_t;

/** @brief Severity of a log line; LOG_LEVEL_OFF disables logging. */
enum log_level_t
{
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARN,
	LOG_LEVEL_OFF,
};

/** @brief Line format: plain text, or one JSON object per line. */
enum log_format_t
{
	LOG_FORMAT_TEXT,
	LOG_FORMAT_JSON,	/**< {"ts":...,"level":...,"msg":...} plus event fields */
};

/**
 * @brief Receives one formatted line without a trailing newline.
 *
 * Called synchronously from the thread that logs; `message` is only valid during the call.
 */
typedef void (*log_sink_t)(void* ctx, log_level_t level, const char* message);

/**
 * @brief Process-wide logging.
 *
 * By default INFO and WARN lines go to stderr with a UTC timestamp. A NULL sink restores the
 * stderr default. Lines below the level are dropped before formatting; messages longer than
 * 1023 bytes are truncated. In LOG_FORMAT_JSON the sink receives the whole JSON object.
 */
void	log_set_sink(log_sink_t sink, void* ctx);
log_sink_t	log_get_sink(void);
void	log_set_level(log_level_t level);
void	log_set_format(log_format_t format);

/**
 * @brief Prefixes every following message with "[prefix] " (NULL or "" = none).
 *
 * The pointer is borrowed: the caller clears it before the text goes away. Servers set it to
 * their id while they run so that the lines of several servers can be told apart.
 */
void	log_set_prefix(const char* prefix);
log_format_t	log_format(void);
bool	log_enabled(log_level_t level);
void	log_write(log_level_t level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief Logs a message with extra JSON members.
 *
 * `fields` is a list of JSON members without braces (e.g. `"event":"request","unit":1`), added
 * to the object in LOG_FORMAT_JSON and ignored in text format.
 */
void	log_write_fields(log_level_t level, const char* fields, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));

/**
 * @brief Writes `text` JSON-escaped (quotes, backslashes, control characters) into `out`.
 *
 * Truncates to fit and always NUL-terminates when `size` > 0.
 * @return Length written, without the NUL.
 */
u32	log_json_escape(char* out, u32 size, const char* text);

#define LOG_INFO(...)	do { if (log_enabled(LOG_LEVEL_INFO)) log_write(LOG_LEVEL_INFO, __VA_ARGS__); } while (0)
#define LOG_WARN(...)	do { if (log_enabled(LOG_LEVEL_WARN)) log_write(LOG_LEVEL_WARN, __VA_ARGS__); } while (0)
#define LOG_DEBUG(...)	do { if (log_enabled(LOG_LEVEL_DEBUG)) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__); } while (0)

#endif
