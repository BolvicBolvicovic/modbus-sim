/**
 * @file
 * @brief Windows clocks (QueryPerformanceCounter, GetSystemTimePreciseAsFileTime).
 */
#include "os/os_time.h"
#include <windows.h>
#include <time.h>

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

s64
os_wall_ms(void)
{
	FILETIME	ft;
	ULARGE_INTEGER	t;

	GetSystemTimePreciseAsFileTime(&ft);
	t.LowPart  = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	/* FILETIME counts 100 ns from 1601; the Unix epoch is 11644473600 s later. */
	return (s64)(t.QuadPart / 10000) - 11644473600000LL;
}

bool
os_civil_from_unix(s64 unix_s, bool utc, os_civil_t* out)
{
	time_t		t = (time_t)unix_s;
	struct tm	tm;

	if ((s64)t != unix_s ||
	    (utc ? gmtime_s(&tm, &t) : localtime_s(&tm, &t)) != 0)
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
