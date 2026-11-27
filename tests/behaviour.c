#include "vtest.h"
#include "device/behaviour.h"
#include "device/encode.h"
#include "vlib/macros.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a, b)	(fabs((a) - (b)) < 1e-9)

TEST(RampValuesAtKnownTimes)
{
	static const struct {
		const char*	name;
		ramp_params_t	r;
		u64		start;
		u32		count;
		struct { u64 now; f64 want; } at[5];
	} cases[] = {
		{ "wrap restarts 0 10 20 30 0", { .from = 0, .to = 30, .step = 10, .every_ms = 1000, .mode = RAMP_WRAP }, 0, 5,
		  { { 0, 0 }, { 999, 0 }, { 1000, 10 }, { 3000, 30 }, { 4000, 0 } } },
		{ "bounce walks back", { .from = 0, .to = 30, .step = 10, .every_ms = 1000, .mode = RAMP_BOUNCE }, 0, 4,
		  { { 3000, 30 }, { 4000, 20 }, { 6000, 0 }, { 7000, 10 } } },
		{ "hold stops", { .from = 0, .to = 30, .step = 10, .every_ms = 1000, .mode = RAMP_HOLD }, 0, 2,
		  { { 3000, 30 }, { 999000, 30 } } },
		{ "goes down when to is below from", { .from = 100, .to = 80, .step = 10, .every_ms = 1000, .mode = RAMP_HOLD }, 0, 2,
		  { { 1000, 90 }, { 5000, 80 } } },
		{ "never passes to", { .from = 0, .to = 25, .step = 10, .every_ms = 1000, .mode = RAMP_HOLD }, 0, 1,
		  { { 99000, 20 } } },
		{ "before start shows from", { .from = 5, .to = 50, .step = 5, .every_ms = 1000, .mode = RAMP_WRAP }, 10000, 1,
		  { { 2000, 5 } } },
		{ "from equals to is constant", { .from = 7, .to = 7, .step = 1, .every_ms = 1000, .mode = RAMP_BOUNCE }, 0, 1,
		  { { 12345, 7 } } },
		{ "bounce just below the limit", { .from = 0, .to = 0x1p62, .step = 1, .every_ms = 1000, .mode = RAMP_BOUNCE }, 0, 3,
		  { { 0, 0 }, { 1000, 1 }, { 2000, 2 } } },
		/* validation rejects these; the runtime guard must not crash */
		{ "bounce last == 2^63", { .from = 0, .to = 0x1p63, .step = 1, .every_ms = 1000, .mode = RAMP_BOUNCE }, 0, 2,
		  { { 0, 0 }, { 1000, 0 } } },
		{ "bounce above the u64 limit", { .from = 0, .to = 0x1p64, .step = 1, .every_ms = 1000, .mode = RAMP_BOUNCE }, 0, 2,
		  { { 0, 0 }, { 1000, 0 } } },
	};

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
		for (u32 j = 0; j < cases[i].count; j++)
		{
			bool ok = NEAR(ramp_value(&cases[i].r, cases[i].start, cases[i].at[j].now),
				       cases[i].at[j].want);

			if (!ok)
				fprintf(stderr, "ramp '%s' at %llu\n", cases[i].name,
					(unsigned long long)cases[i].at[j].now);
			ASSERT_EQ(ok);
		}
}

static model_t
make_lag(f64 tau_ms, f64 initial)
{
	return (model_t){ .shape = MODEL_LAG, .gain = 1, .tau_ms = (u32)tau_ms, .initial = initial,
			  .has_initial = true, .target_value = 100 };
}

TEST(SineFollowsFormulaPhaseAndStart)
{
	static const model_t plain = { .shape = MODEL_SINE, .offset = 10, .amplitude = 4, .period_ms = 4000 };
	static const model_t phased = { .shape = MODEL_SINE, .amplitude = 1, .period_ms = 1000, .phase_deg = 90 };
	static const struct {
		const model_t*	m;
		u64		start, now;
		f64		want;
	} cases[] = {
		{ &plain, 0, 0, 10 }, { &plain, 0, 1000, 14 }, { &plain, 0, 2000, 10 },
		{ &plain, 0, 3000, 6 }, { &plain, 0, 5000, 14 },
		/* time counts from the start; before it the value is the one at the start */
		{ &plain, 1000, 2000, 14 }, { &plain, 1000, 500, 10 },
		{ &phased, 0, 0, 1 }, { &phased, 0, 500, -1 },
	};

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		model_t	m = *cases[i].m;

		bool	ok = NEAR(model_value(&m, 0, 0, cases[i].start, cases[i].now, 0), cases[i].want);

		if (!ok)
			fprintf(stderr, "sine case %u\n", i);
		ASSERT_EQ(ok);
	}
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
