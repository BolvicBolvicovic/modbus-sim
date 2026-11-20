#ifndef BEHAVIOUR_H
#define BEHAVIOUR_H

#include "vlib/c_types.h"

/* Name: ramp_mode_t
 * Description: what a ramp does when it reaches its end.
 * - RAMP_WRAP  : jump back to `from` (sawtooth)
 * - RAMP_BOUNCE: turn around and walk back (triangle wave)
 * - RAMP_HOLD  : stay on the last value
 * Keep the order: the config parser maps names to these values by index.
 * */
typedef enum ramp_mode_t ramp_mode_t;
enum ramp_mode_t
{
	RAMP_WRAP,
	RAMP_BOUNCE,
	RAMP_HOLD,
};

/* Name: ramp_params_t
 * Description: a value that moves from `from` towards `to` by `step`
 * every `every_ms` milliseconds. `step` is always positive; the direction
 * comes from `from` and `to`. The last value is never beyond `to`.
 * */
typedef struct ramp_params_t ramp_params_t;
struct ramp_params_t
{
	f64		from;
	f64		to;
	f64		step;
	u32		every_ms;
	ramp_mode_t	mode;
};

/* Name: ramp_value
 * Description: the ramp value at `now_ms`, for a ramp that started at `start_ms`.
 * Stateless: the answer only depends on the arguments, so it is reproducible.
 * */
f64	ramp_value(const ramp_params_t* ramp, u64 start_ms, u64 now_ms);

#endif
