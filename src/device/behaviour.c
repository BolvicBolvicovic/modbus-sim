#include "device/behaviour.h"
#include "vlib/macros.h"
#include <math.h>

/* Index of the value to show after `ticks` steps, for a bouncing ramp
 * that has `last + 1` distinct values (indexes 0..last). */
static u64
bounce_index(u64 ticks, u64 last)
{
	u64	phase = 0;

	if (last == 0)
		return 0;

	phase = ticks % (2 * last);
	return phase <= last ? phase : 2 * last - phase;
}

f64
ramp_value(const ramp_params_t* ramp, u64 start_ms, u64 now_ms)
{
	f64	span      = ramp->to - ramp->from;
	f64	direction = span < 0 ? -1.0 : 1.0;
	u64	last      = (u64)(fabs(span) / ramp->step + 1e-9);
	u64	elapsed   = now_ms > start_ms ? now_ms - start_ms : 0;
	u64	ticks     = elapsed / ramp->every_ms;
	u64	index     = 0;

	switch (ramp->mode)
	{
	case RAMP_WRAP:
		index = ticks % (last + 1);
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
