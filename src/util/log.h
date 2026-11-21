#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

/* Logging goes to stderr. DEBUG lines only print when verbose is on. */
void	log_set_verbose(bool verbose);
bool	log_verbose(void);
void	log_write(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_INFO(...)	log_write("info", __VA_ARGS__)
#define LOG_WARN(...)	log_write("warn", __VA_ARGS__)
#define LOG_DEBUG(...)	do { if (log_verbose()) log_write("debug", __VA_ARGS__); } while (0)

#endif
