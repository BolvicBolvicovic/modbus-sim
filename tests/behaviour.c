#include "vtest.h"
#include "device/behaviour.h"
#include "device/encode.h"
#include "vlib/macros.h"
#include <float.h>
#include <math.h>
#include <string.h>

#define NEAR(a, b)	(fabs((a) - (b)) < 1e-9)

static ramp_params_t
make_ramp(f64 from, f64 to, f64 step, ramp_mode_t mode)
{
	return (ramp_params_t){ .from = from, .to = to, .step = step, .every_ms = 1000, .mode = mode };
}

TEST(RampWrapRestarts)
{
	ramp_params_t	r = make_ramp(0, 30, 10, RAMP_WRAP);	/* 0 10 20 30 0 ... */

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 0), 0));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 999), 0));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 1000), 10));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 3000), 30));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 4000), 0));
}

TEST(RampBounceWalksBack)
{
	ramp_params_t	r = make_ramp(0, 30, 10, RAMP_BOUNCE);	/* 0 10 20 30 20 10 0 10 */

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 3000), 30));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 4000), 20));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 6000), 0));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 7000), 10));
}

TEST(RampHoldStops)
{
	ramp_params_t	r = make_ramp(0, 30, 10, RAMP_HOLD);

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 3000), 30));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 999000), 30));
}

TEST(RampGoesDownWhenToIsBelowFrom)
{
	ramp_params_t	r = make_ramp(100, 80, 10, RAMP_HOLD);

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 1000), 90));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 5000), 80));
}

TEST(RampNeverPassesTo)
{
	ramp_params_t	r = make_ramp(0, 25, 10, RAMP_HOLD);	/* last value is 20 */

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 99000), 20));
}

TEST(RampBeforeStartShowsFrom)
{
	ramp_params_t	r = make_ramp(5, 50, 5, RAMP_WRAP);

	ASSERT_EQ(NEAR(ramp_value(&r, 10000, 2000), 5));
}

TEST(RampFromEqualsToIsConstant)
{
	ramp_params_t	r = make_ramp(7, 7, 1, RAMP_BOUNCE);

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 12345), 7));
}

TEST(RampBounceJustBelowLimit)
{
	ramp_params_t	r = make_ramp(0, 0x1p62, 1, RAMP_BOUNCE);

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 0), 0));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 1000), 1));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 2000), 2));
}

TEST(RampBounceLargeStepCountIsRobust)
{
	ramp_params_t	r = make_ramp(0, 0x1p63, 1, RAMP_BOUNCE);

	/* last == 2^63: validation must reject this, but the runtime guard must not crash. */
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 0), 0));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 1000), 0));
}

TEST(RampBounceAboveU64LimitIsRobust)
{
	ramp_params_t	r = make_ramp(0, 0x1p64, 1, RAMP_BOUNCE);

	ASSERT_EQ(NEAR(ramp_value(&r, 0, 0), 0));
	ASSERT_EQ(NEAR(ramp_value(&r, 0, 1000), 0));
}

TEST(EncodeIntegers)
{
	u16	w[2] = { 0, 0 };

	ASSERT_EQ(encode_value(VT_U16, WORDS_HIGH_FIRST, 70000, w) == 1);
	ASSERT_EQ(w[0] == 65535);
	encode_value(VT_S16, WORDS_HIGH_FIRST, -2, w);
	ASSERT_EQ(w[0] == 0xFFFE);
	encode_value(VT_U16, WORDS_HIGH_FIRST, 12.6, w);
	ASSERT_EQ(w[0] == 13);
	encode_value(VT_U16, WORDS_HIGH_FIRST, NAN, w);
	ASSERT_EQ(w[0] == 0);
	encode_value(VT_S32, WORDS_HIGH_FIRST, INFINITY, w);
	ASSERT_EQ(w[0] == 0x7FFF && w[1] == 0xFFFF);
	encode_value(VT_U32, WORDS_HIGH_FIRST, -INFINITY, w);
	ASSERT_EQ(w[0] == 0x0000 && w[1] == 0x0000);
}

TEST(EncodeThirtyTwoBitWordOrder)
{
	u16	w[2] = { 0, 0 };

	ASSERT_EQ(encode_value(VT_U32, WORDS_HIGH_FIRST, 0x12345678, w) == 2);
	ASSERT_EQ(w[0] == 0x1234 && w[1] == 0x5678);
	encode_value(VT_U32, WORDS_LOW_FIRST, 0x12345678, w);
	ASSERT_EQ(w[0] == 0x5678 && w[1] == 0x1234);
	encode_value(VT_S32, WORDS_HIGH_FIRST, -1, w);
	ASSERT_EQ(w[0] == 0xFFFF && w[1] == 0xFFFF);
}

TEST(EncodeFloat32)
{
	u16	w[2] = { 0, 0 };
	u32	raw  = 0;
	f32	real = 0;

	encode_value(VT_F32, WORDS_HIGH_FIRST, 1.0, w);	/* 0x3F800000 */
	ASSERT_EQ(w[0] == 0x3F80 && w[1] == 0x0000);
	encode_value(VT_F32, WORDS_LOW_FIRST, 1.0, w);
	ASSERT_EQ(w[0] == 0x0000 && w[1] == 0x3F80);

	encode_value(VT_F32, WORDS_HIGH_FIRST, NAN, w);
	raw = ((u32)w[0] << 16) | w[1];
	memcpy(&real, &raw, sizeof(real));
	ASSERT_EQ(isnan(real));

	encode_value(VT_F32, WORDS_HIGH_FIRST, INFINITY, w);
	ASSERT_EQ(w[0] == 0x7F80 && w[1] == 0x0000);
	encode_value(VT_F32, WORDS_HIGH_FIRST, -INFINITY, w);
	ASSERT_EQ(w[0] == 0xFF80 && w[1] == 0x0000);

	encode_value(VT_F32, WORDS_HIGH_FIRST, (f64)FLT_MAX * 2.0, w);
	ASSERT_EQ(w[0] == 0x7F80 && w[1] == 0x0000);
	encode_value(VT_F32, WORDS_HIGH_FIRST, -((f64)FLT_MAX * 2.0), w);
	ASSERT_EQ(w[0] == 0xFF80 && w[1] == 0x0000);
}

TEST(EncodeBool)
{
	u16	w[2] = { 0, 0 };

	encode_value(VT_BOOL, WORDS_HIGH_FIRST, 5, w);
	ASSERT_EQ(w[0] == 1);
	encode_value(VT_BOOL, WORDS_HIGH_FIRST, 0, w);
	ASSERT_EQ(w[0] == 0);
}

static model_t
make_lag(f64 tau_ms, f64 initial)
{
	return (model_t){ .shape = MODEL_LAG, .gain = 1, .tau_ms = (u32)tau_ms, .initial = initial,
			  .has_initial = true, .target_value = 100 };
}

TEST(SineFollowsFormulaAtKnownTimes)
{
	model_t	m = { .shape = MODEL_SINE, .offset = 10, .amplitude = 4, .period_ms = 4000 };

	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 0, 0), 10));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 1000, 0), 14));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 2000, 0), 10));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 3000, 0), 6));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 5000, 0), 14));
	/* time counts from the start; before it the value is the one at the start */
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 1000, 2000, 0), 14));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 1000, 500, 0), 10));
}

TEST(SinePhaseShiftsTheWave)
{
	model_t	m = { .shape = MODEL_SINE, .amplitude = 1, .period_ms = 1000, .phase_deg = 90 };

	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 0, 0), 1));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 500, 0), -1));
}

TEST(LagReachesOneMinusInverseEAfterTau)
{
	model_t	m = make_lag(1000, 0);

	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 0, 0), 0));
	ASSERT_EQ(fabs(model_value(&m, 0, 0, 0, 1000, 0) - 100 * (1 - exp(-1.0))) < 1e-9);
	ASSERT_EQ(fabs(model_value(&m, 0, 0, 0, 1000000, 0) - 100) < 1e-6);
}

TEST(LagDoesNotDependOnPollRate)
{
	model_t	once = make_lag(5000, 20);
	model_t	many = make_lag(5000, 20);
	f64	y_once = 0;
	f64	y_many = 0;

	model_value(&once, 0, 0, 0, 0, 0);
	y_once = model_value(&once, 0, 0, 0, 7300, 0);
	model_value(&many, 0, 0, 0, 0, 0);
	for (u64 t = 13; t < 7300; t += 13)
		model_value(&many, 0, 0, 0, t, 0);
	y_many = model_value(&many, 0, 0, 0, 7300, 0);
	ASSERT_EQ(fabs(y_once - y_many) < 1e-9);
}

TEST(LagStartsAtGoalWithoutInitialAndAppliesGainAndOffset)
{
	model_t	m = { .shape = MODEL_LAG, .gain = 2, .offset = 5, .tau_ms = 1000,
		      .target_is_point = true };

	ASSERT_EQ(NEAR(model_value(&m, 0, 10, 0, 0, 0), 25));
	ASSERT_EQ(NEAR(model_value(&m, 0, 10, 0, 50000, 0), 25));
	model_reset(&m);
	ASSERT_EQ(NEAR(model_value(&m, 0, 20, 60000, 60000, 0), 45));
}

TEST(NoiseStaysInBoundsAndIsDeterministic)
{
	model_t	m = { .shape = MODEL_BASE, .noise = { .amplitude = 2, .every_ms = 100 } };
	bool	varies = false;
	f64	first = model_value(&m, 50, 0, 0, 0, 7);

	for (u64 slot = 0; slot < 500; slot++)
	{
		f64	v = model_value(&m, 50, 0, 0, slot * 100, 7);

		ASSERT_EQ(v >= 48 && v <= 52);
		ASSERT_EQ(v == model_value(&m, 50, 0, 0, slot * 100, 7));
		ASSERT_EQ(v == model_value(&m, 50, 0, 0, slot * 100 + 99, 7));
		varies = varies || v != first;
	}
	ASSERT_EQ(varies);
	/* a different point gets a different sequence */
	ASSERT_EQ(model_value(&m, 50, 0, 0, 0, 8) != first);
}

TEST(GaussianNoiseIsClampedToFourSigma)
{
	model_t	m = { .shape = MODEL_BASE,
		      .noise = { .amplitude = 3, .every_ms = 1, .gaussian = true } };
	f64	sum = 0;

	for (u64 t = 0; t < 20000; t++)
	{
		f64	v = model_value(&m, 0, 0, 0, t, 1);

		ASSERT_EQ(fabs(v) <= 12.0 + 1e-9);
		sum += v;
	}
	ASSERT_EQ(fabs(sum / 20000) < 0.2);
}

TEST(NoiseIsAddedToLagOutputOnly)
{
	model_t	m = make_lag(1000, 0);
	model_t	plain = make_lag(1000, 0);

	m.noise = (struct model_noise_t){ .amplitude = 1, .every_ms = 10 };
	for (u64 t = 0; t <= 5000; t += 100)
	{
		f64	noisy = model_value(&m, 0, 0, 0, t, 3);
		f64	clean = model_value(&plain, 0, 0, 0, t, 3);

		ASSERT_EQ(fabs(noisy - clean) <= 1.0);
		ASSERT_EQ(fabs(m.lag_y - plain.lag_y) < 1e-12);
	}
}

TEST(DegradationDriftAddsPerHourAfterTheStart)
{
	model_t	m = { .shape = MODEL_BASE,
		      .degradation = { .after_ms = 3600000, .drift_per_hour = 2 } };

	ASSERT_EQ(NEAR(model_value(&m, 10, 0, 0, 0, 0), 10));
	ASSERT_EQ(NEAR(model_value(&m, 10, 0, 0, 3600000, 0), 10));
	ASSERT_EQ(NEAR(model_value(&m, 10, 0, 0, 3600000 + 1800000, 0), 11));
	ASSERT_EQ(NEAR(model_value(&m, 10, 0, 0, 3 * 3600000, 0), 14));
	/* the timeline counts from the device start */
	ASSERT_EQ(NEAR(model_value(&m, 10, 0, 1000, 1000 + 2 * 3600000, 0), 12));
	m.degradation.drift_per_hour = -4;
	ASSERT_EQ(NEAR(model_value(&m, 10, 0, 0, 2 * 3600000, 0), 6));
}

TEST(DegradationNoiseGrowthWidensTheNoiseOverTime)
{
	model_t	m = { .shape = MODEL_BASE,
		      .degradation = { .after_ms = 1000000, .noise_growth_per_hour = 3.6 } };
	f64	largest_early = 0;
	f64	largest_late = 0;

	for (u64 t = 0; t < 1000000; t += 1000)
		ASSERT_EQ(NEAR(model_value(&m, 50, 0, 0, t, 5), 50));
	/* one hour past `after`: amplitude 3.6 (4.2 ten minutes later), uniform, one new sample per second */
	for (u64 t = 1000000 + 3600000; t < 1000000 + 3600000 + 600000; t += 1000)
	{
		f64	d = fabs(model_value(&m, 50, 0, 0, t, 5) - 50);

		ASSERT_EQ(d <= 4.2 + 1e-9);
		largest_late = MAX(largest_late, d);
	}
	ASSERT_EQ(largest_late > 2.0);
	/* a point's own noise keeps its distribution and adds to the growth */
	m.noise = (struct model_noise_t){ .amplitude = 1, .every_ms = 10 };
	for (u64 t = 0; t < 1000000; t += 997)
	{
		f64	d = fabs(model_value(&m, 50, 0, 0, t, 5) - 50);

		ASSERT_EQ(d <= 1.0 + 1e-9);
		largest_early = MAX(largest_early, d);
	}
	ASSERT_EQ(largest_early > 0.5);
	for (u64 t = 1000000 + 3600000; t < 1000000 + 3600000 + 100000; t += 10)
		ASSERT_EQ(fabs(model_value(&m, 50, 0, 0, t, 5) - 50) <= 4.8 + 1e-9);
}

TEST(DegradationStuckHoldsTheFirstValueFromItsTimeAndResets)
{
	model_t	m = { .shape = MODEL_SINE, .amplitude = 10, .period_ms = 4000,
		      .degradation = { .has_stuck = true, .stuck_after_ms = 1500 } };
	f64	held = 0;

	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 1000, 0), 10));
	/* the first evaluation at or after 1500 ms is the one that sticks */
	held = model_value(&m, 0, 0, 0, 2000, 0);
	ASSERT_EQ(NEAR(held, 0));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 3000, 0), held));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 9999, 0), held));
	model_reset(&m);
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 3000, 0), -10));
	ASSERT_EQ(NEAR(model_value(&m, 0, 0, 0, 3500, 0), -10));
}

TEST(DegradationStuckKeepsDriftAndNoiseAtTheFreezeInstant)
{
	model_t	m = { .shape = MODEL_BASE,
		      .noise = { .amplitude = 1, .every_ms = 10 },
		      .degradation = { .drift_per_hour = 3600, .has_stuck = true,
				       .stuck_after_ms = 2000 } };
	f64	held = model_value(&m, 0, 0, 0, 2000, 9);

	ASSERT_EQ(fabs(held - 2) <= 1.0);
	for (u64 t = 2010; t < 20000; t += 10)
		ASSERT_EQ(model_value(&m, 0, 0, 0, t, 9) == held);
}

TEST(DegradationFlagsDescribeTheEffects)
{
	model_t	m = { 0 };

	ASSERT_EQ(!model_has_degradation(&m));
	m.degradation.after_ms = 5;
	ASSERT_EQ(!model_has_degradation(&m));
	m.degradation.drift_per_hour = -1;
	ASSERT_EQ(model_has_degradation(&m));
	m = (model_t){ 0 };
	m.degradation.has_fail = true;
	ASSERT_EQ(model_has_degradation(&m));
}
