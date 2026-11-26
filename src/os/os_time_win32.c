/**
 * @file
 * @brief Windows clocks (QueryPerformanceCounter, GetSystemTimePreciseAsFileTime).
 */
#include "os/os_time.h"
#include <windows.h>

static u64
ticks_scaled(u64 per_second)
{
	LARGE_INTEGER	freq;
	LARGE_INTEGER	now;
	u64		whole;
	u64		rest;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	whole = (u64)now.QuadPart / (u64)freq.QuadPart;
	rest  = (u64)now.QuadPart % (u64)freq.QuadPart;
	/* Split the division so the product cannot overflow after weeks of uptime. */
	return whole * per_second + rest * per_second / (u64)freq.QuadPart;
}

u64
os_now_ms(void)
{
	return ticks_scaled(1000);
}

u64
os_now_us(void)
{
	return ticks_scaled(1000000);
}

bool
os_utc_now(os_utc_t* out)
{
	FILETIME	ft;
	SYSTEMTIME	st;

	GetSystemTimePreciseAsFileTime(&ft);
	if (!FileTimeToSystemTime(&ft, &st))
		return false;
	*out = (os_utc_t){
		.year  = st.wYear,
		.month = st.wMonth,
		.day   = st.wDay,
		.hour  = st.wHour,
		.min   = st.wMinute,
		.sec   = st.wSecond,
		.msec  = st.wMilliseconds,
	};
	return true;
}
