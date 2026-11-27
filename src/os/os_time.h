/**
 * @file
 * @brief Monotonic and wall clocks.
 */
#ifndef OS_TIME_H
#define OS_TIME_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct os_utc_t os_utc_t;

/** @brief Broken-down UTC wall time. */
struct os_utc_t
{
	u32	year;
	u32	month;	/**< 1-12 */
	u32	day;
	u32	hour;
	u32	min;
	u32	sec;
	u32	msec;
};

/** @brief Monotonic time in milliseconds; never goes backwards. */
u64	os_now_ms(void);

/** @brief Monotonic time in microseconds; never goes backwards. */
u64	os_now_us(void);

typedef struct os_civil_t os_civil_t;

/** @brief Broken-down calendar time. */
struct os_civil_t
{
	u32	year;
	u32	month;		/**< 1-12 */
	u32	day;		/**< 1-31 */
	u32	weekday;	/**< ISO: Monday 1 ... Sunday 7 */
	u32	hour;
	u32	min;
	u32	sec;
};

/** @brief Wall clock: milliseconds since the Unix epoch (UTC). */
s64	os_wall_ms(void);

/** @brief Breaks `unix_s` seconds since the epoch into UTC or local calendar fields; false on failure. */
bool	os_civil_from_unix(s64 unix_s, bool utc, os_civil_t* out);

/** @brief Current UTC wall time; false when the clock cannot be read. */
bool	os_utc_now(os_utc_t* out);

#endif
