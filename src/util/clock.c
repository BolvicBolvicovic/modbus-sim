#include "util/clock.h"
#include <time.h>

u64
clock_now_ms(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}
