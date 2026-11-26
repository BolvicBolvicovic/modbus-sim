/**
 * @file
 * @brief Time-based point behaviours (ramps, sine, lag) and the noise modifier.
 */
#ifndef BEHAVIOUR_H
#define BEHAVIOUR_H

#include "vlib/c_types.h"
#include <stdbool.h>

/**
 * @brief What a ramp does when it reaches its end.
 *
 * - RAMP_WRAP  : jump back to `from` (sawtooth)
 * - RAMP_BOUNCE: turn around and walk back (triangle wave)
 * - RAMP_HOLD  : stay on the last value
 * Keep the order: the config parser maps names to these values by index.
 */
typedef enum ramp_mode_t ramp_mode_t;
enum ramp_mode_t
{
	RAMP_WRAP,
	RAMP_BOUNCE,
	RAMP_HOLD,
};

/**
 * @brief A value that moves from `from` towards `to` by `step` every `every_ms` milliseconds.
 *
 * `step` is always positive; the direction comes from `from` and `to`. The last value is never
 * beyond `to`.
 */
typedef struct ramp_params_t ramp_params_t;
struct ramp_params_t
{
	f64		from;
	f64		to;
	f64		step;
	u32		every_ms;
	ramp_mode_t	mode;
};

/**
 * @brief The ramp value at `now_ms`, for a ramp that started at `start_ms`.
 *
 * Stateless: the answer only depends on the arguments, so it is reproducible.
 */
f64	ramp_value(const ramp_params_t* ramp, u64 start_ms, u64 now_ms);

/** @brief What a model computes before the noise modifier is added. */
typedef enum model_shape_t model_shape_t;
enum model_shape_t
{
	MODEL_BASE,	/**< the base value passed to model_value(): a static value or a ramp */
	MODEL_SINE,	/**< offset + amplitude * sin(2 pi t / period + phase) */
	MODEL_LAG,	/**< first-order lag towards gain * target + offset */
};

/**
 * @brief A modeled value: one shape plus the optional noise modifier, evaluated by
 *        model_value().
 *
 * Fields are grouped by the shape that uses them. Add a modifier by adding a struct here and
 * one step to model_value().
 */
typedef struct model_t model_t;
struct model_t
{
	model_shape_t	shape;
	f64		offset;		/**< sine: centre value; lag: added to gain * target */
	f64		amplitude;	/**< sine */
	f64		phase_deg;	/**< sine */
	u32		period_ms;	/**< sine: 1..86400000 */
	f64		gain;		/**< lag */
	f64		initial;	/**< lag, used when has_initial */
	bool		has_initial;	/**< lag: otherwise it starts at its goal */
	u32		tau_ms;		/**< lag: time constant, 1..86400000 */
	bool		target_is_point;/**< lag: target is a point, otherwise `target_value` */
	u32		target_space;	/**< lag: space_kind_t of the target point */
	u32		target_address;	/**< lag: first address of the target point */
	f64		target_value;	/**< lag: constant target */
	f64		lag_y;		/**< lag state: value without noise at lag_ms */
	u64		lag_ms;		/**< lag state: time of the last evaluation */
	bool		lag_started;	/**< lag state: false until the first evaluation */
	struct model_noise_t
	{
		f64	amplitude;	/**< 0 = no noise; uniform +-A or gaussian sigma A (max 4A) */
		u32	every_ms;	/**< a new sample every this many ms, 1..86400000 */
		bool	gaussian;
	} noise;
};

/** @brief True if the model adds a lag target read from a point. */
bool	model_needs_target(const model_t* model);

/** @brief Forgets the lag state: the next evaluation starts at `initial` (or the goal). */
void	model_reset(model_t* model);

/**
 * @brief The model value at `now_ms`, for a point that started at `start_ms`.
 *
 * `base` is the static or ramp value (MODEL_BASE) and `target` the current value of the lag
 * target point (MODEL_LAG; a constant target is used as is). Sine, base and noise are
 * stateless. Lag advances its state by the exact solution over the time since its last
 * evaluation, so polling rate does not matter while the goal is constant between polls; a
 * goal that changes between polls is sampled at poll times. Noise is sample-and-hold, a pure
 * function of (`noise_key`, floor((now - start) / every_ms)), and is added to the output only.
 */
f64	model_value(model_t* model, f64 base, f64 target, u64 start_ms, u64 now_ms,
		    u64 noise_key);

#endif
