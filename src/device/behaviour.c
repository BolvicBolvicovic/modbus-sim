/**
 * @file
 * @brief Ramp behaviour: the value of a ramp at a given time.
 */
#include "device/behaviour.h"
#include "util/rng.h"
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

enum { NOISE_GAUSS_LIMIT = 4 };

bool
model_needs_target(const model_t* model)
{
	return model->shape == MODEL_LAG && model->target_is_point;
}

bool
model_has_degradation(const model_t* model)
{
	return model->degradation.drift_per_hour != 0 ||
	       model->degradation.noise_growth_per_hour > 0 ||
	       model->degradation.has_stuck ||
	       model->degradation.has_fail;
}

void
model_reset(model_t* model)
{
	model->lag_started = false;
	model->degradation.stuck_started = false;
}

/** @brief Uniform number in [0, 1) from the top 53 bits of a random word. */
static f64
unit_random(rng_t* rng)
{
	return (f64)(rng_next(rng) >> 11) * 0x1p-53;
}

static f64
noise_value(const struct model_noise_t* noise, u64 key, u64 slot)
{
	rng_t	rng;
	f64	u1 = 0;
	f64	z  = 0;

	/* Two seeding rounds keep (key, slot) pairs from colliding by simple addition. */
	rng_seed(&rng, key);
	rng_seed(&rng, rng_next(&rng) + slot);
	u1 = unit_random(&rng);
	if (!noise->gaussian)
		return noise->amplitude * (2.0 * u1 - 1.0);

	/* Box-Muller; 1 - u1 is in (0, 1] so log() is finite. */
	z = sqrt(-2.0 * log(1.0 - u1)) * cos(2.0 * M_PI * unit_random(&rng));
	z = MAX(-NOISE_GAUSS_LIMIT, MIN(z, NOISE_GAUSS_LIMIT));
	return noise->amplitude * z;
}

f64
model_value(model_t* model, f64 base, f64 target, u64 start_ms, u64 now_ms, u64 noise_key)
{
	u64	elapsed = now_ms > start_ms ? now_ms - start_ms : 0;
	f64	value   = base;
	struct model_noise_t	noise = model->noise;
	struct model_degradation_t*	deg = &model->degradation;

	switch (model->shape)
	{
	case MODEL_BASE:
		break;
	case MODEL_SINE:
	{
		f64	turns = (f64)(elapsed % model->period_ms) / (f64)model->period_ms;

		value = model->offset +
			model->amplitude * sin(2.0 * M_PI * turns + model->phase_deg * M_PI / 180.0);
		break;
	}
	case MODEL_LAG:
	{
		f64	goal = model->gain * (model->target_is_point ? target : model->target_value) +
			       model->offset;
		f64	dt   = 0;

		if (!model->lag_started)
		{
			model->lag_y       = model->has_initial ? model->initial : goal;
			model->lag_ms      = now_ms;
			model->lag_started = true;
		}
		dt = now_ms > model->lag_ms ? (f64)(now_ms - model->lag_ms) : 0.0;
		model->lag_y += (goal - model->lag_y) * (1.0 - exp(-dt / (f64)model->tau_ms));
		if (now_ms > model->lag_ms)
			model->lag_ms = now_ms;
		value = model->lag_y;
		break;
	}
	}

	if (elapsed > deg->after_ms)
	{
		f64	hours = (f64)(elapsed - deg->after_ms) / 3600000.0;

		value += deg->drift_per_hour * hours;
		if (deg->noise_growth_per_hour > 0)
		{
			if (noise.amplitude <= 0)
			{
				noise.every_ms = 1000;
				noise.gaussian = false;
			}
			noise.amplitude += deg->noise_growth_per_hour * hours;
		}
	}

	if (noise.amplitude > 0)
		value += noise_value(&noise, noise_key, elapsed / noise.every_ms);

	if (deg->has_stuck &&
	    elapsed >= deg->stuck_after_ms)
	{
		if (!deg->stuck_started)
		{
			deg->stuck_value   = value;
			deg->stuck_started = true;
		}
		value = deg->stuck_value;
	}
	return value;
}
