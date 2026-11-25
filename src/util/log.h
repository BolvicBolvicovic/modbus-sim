#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

typedef enum log_level_t log_level_t;
enum log_level_t
{
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARN,
	LOG_LEVEL_OFF,
};

/* Receives one formatted line without a trailing newline. Called synchronously
 * from the thread that logs; `message` is only valid during the call. */
typedef void (*log_sink_t)(void* ctx, log_level_t level, const char* message);

/* Process-wide logging. By default INFO and WARN lines go to stderr with a UTC
 * timestamp. A NULL sink restores the stderr default. Lines below the level are
 * dropped before formatting; messages longer than 1023 bytes are truncated. */
void	log_set_sink(log_sink_t sink, void* ctx);
void	log_set_level(log_level_t level);
bool	log_enabled(log_level_t level);
void	log_write(log_level_t level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_INFO(...)	do { if (log_enabled(LOG_LEVEL_INFO)) log_write(LOG_LEVEL_INFO, __VA_ARGS__); } while (0)
#define LOG_WARN(...)	do { if (log_enabled(LOG_LEVEL_WARN)) log_write(LOG_LEVEL_WARN, __VA_ARGS__); } while (0)
#define LOG_DEBUG(...)	do { if (log_enabled(LOG_LEVEL_DEBUG)) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__); } while (0)

#endif
