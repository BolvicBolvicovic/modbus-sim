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
