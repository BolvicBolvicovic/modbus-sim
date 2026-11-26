/**
 * @file
 * @brief Ramp behaviour: the value of a ramp at a given time.
 */
#include "device/behaviour.h"
#include "vlib/macros.h"
#include <math.h>

/**
 * @brief Index of the value to show after `ticks` steps, for a bouncing ramp that has `last +
 *        1` distinct values (indexes 0..last).
 */
static u64
bounce_index(u64 ticks, u64 last)
{
	u64	phase  = 0;
	u64	period = 0;

	if (last == 0 || last > (UINT64_MAX / 2))
		return 0;

	period = 2 * last;
	phase  = ticks % period;
	return phase <= last ? phase : period - phase;
}

f64
ramp_value(const ramp_params_t* ramp, u64 start_ms, u64 now_ms)
{
	f64	span      = ramp->to - ramp->from;
	f64	direction = span < 0 ? -1.0 : 1.0;
	f64	steps     = fabs(span) / ramp->step + 1e-9;
	u64	last      = (!isfinite(steps) || steps >= 0x1p64) ? UINT64_MAX : (u64)steps;
	u64	elapsed   = now_ms > start_ms ? now_ms - start_ms : 0;
	u64	ticks     = elapsed / ramp->every_ms;
	u64	index     = 0;

	switch (ramp->mode)
	{
	case RAMP_WRAP:
		index = last == UINT64_MAX ? 0 : ticks % (last + 1);
		break;
	case RAMP_BOUNCE:
		index = bounce_index(ticks, last);
		break;
	case RAMP_HOLD:
		index = MIN(ticks, last);
		break;
	}

	return ramp->from + direction * ramp->step * (f64)index;
}
