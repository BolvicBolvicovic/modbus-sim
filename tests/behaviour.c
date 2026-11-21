#include "vtest.h"
#include "device/behaviour.h"
#include "device/encode.h"
#include <math.h>

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

	encode_value(VT_F32, WORDS_HIGH_FIRST, 1.0, w);	/* 0x3F800000 */
	ASSERT_EQ(w[0] == 0x3F80 && w[1] == 0x0000);
	encode_value(VT_F32, WORDS_LOW_FIRST, 1.0, w);
	ASSERT_EQ(w[0] == 0x0000 && w[1] == 0x3F80);
}

TEST(EncodeBool)
{
	u16	w[2] = { 0, 0 };

	encode_value(VT_BOOL, WORDS_HIGH_FIRST, 5, w);
	ASSERT_EQ(w[0] == 1);
	encode_value(VT_BOOL, WORDS_HIGH_FIRST, 0, w);
	ASSERT_EQ(w[0] == 0);
}
