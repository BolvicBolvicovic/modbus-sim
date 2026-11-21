#include "util/rng.h"

#define RNG_FALLBACK_SEED	0x9E3779B97F4A7C15ULL

static u64
splitmix64(u64* x)
{
	u64	z = (*x += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static u64
rotl(u64 x, int k)
{
	return (x << k) | (x >> (64 - k));
}

void
rng_seed(rng_t* rng, u64 seed)
{
	u64	x = seed ? seed : RNG_FALLBACK_SEED;

	rng->s[0] = splitmix64(&x);
	rng->s[1] = splitmix64(&x);
	rng->s[2] = splitmix64(&x);
	rng->s[3] = splitmix64(&x);
}

u64
rng_next(rng_t* rng)
{
	const u64	result = rotl(rng->s[1] * 5, 7) * 9;
	const u64	t      = rng->s[1] << 17;

	rng->s[2] ^= rng->s[0];
	rng->s[3] ^= rng->s[1];
	rng->s[1] ^= rng->s[2];
	rng->s[0] ^= rng->s[3];
	rng->s[2] ^= t;
	rng->s[3] = rotl(rng->s[3], 45);
	return result;
}

bool
rng_percent(rng_t* rng, u32 percent)
{
	if (percent >= 100)
		return true;
	if (percent == 0)
		return false;
	/* Map 32 high bits to [0,100) with negligible bias for this use. */
	return ((rng_next(rng) >> 32) * 100ULL >> 32) < percent;
}
