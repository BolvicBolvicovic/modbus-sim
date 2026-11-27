/**
 * @file
 * @brief POSIX clocks (clock_gettime).
 */
#include "os/os_time.h"
#include <time.h>

u64
os_now_ms(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}

u64
os_now_us(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000 + (u64)ts.tv_nsec / 1000;
}

s64
os_wall_ms(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (s64)ts.tv_sec * 1000 + (s64)ts.tv_nsec / 1000000;
}

bool
os_civil_from_unix(s64 unix_s, bool utc, os_civil_t* out)
{
	time_t		t = (time_t)unix_s;
	struct tm	tm;

	if ((s64)t != unix_s ||
	    !(utc ? gmtime_r(&t, &tm) : localtime_r(&t, &tm)))
		return false;
	*out = (os_civil_t){
		.year    = (u32)(tm.tm_year + 1900),
		.month   = (u32)tm.tm_mon + 1,
		.day     = (u32)tm.tm_mday,
		.weekday = tm.tm_wday == 0 ? 7u : (u32)tm.tm_wday,
		.hour    = (u32)tm.tm_hour,
		.min     = (u32)tm.tm_min,
		.sec     = (u32)tm.tm_sec,
	};
	return true;
}

bool
os_utc_now(os_utc_t* out)
{
	struct timespec	ts;
	struct tm	tm;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
	    !gmtime_r(&ts.tv_sec, &tm))
		return false;
	*out = (os_utc_t){
		.year  = (u32)tm.tm_year + 1900,
		.month = (u32)tm.tm_mon + 1,
		.day   = (u32)tm.tm_mday,
		.hour  = (u32)tm.tm_hour,
		.min   = (u32)tm.tm_min,
		.sec   = (u32)tm.tm_sec,
		.msec  = (u32)(ts.tv_nsec / 1000000L),
	};
	return true;
}
