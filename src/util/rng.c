#include "util/rng.h"

#define RNG_FALLBACK_SEED	0x9E3779B97F4A7C15ULL

void
rng_seed(rng_t* rng, u64 seed)
{
	rng->state = seed ? seed : RNG_FALLBACK_SEED;
}

u64
rng_next(rng_t* rng)
{
	u64	x = rng->state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng->state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

bool
rng_percent(rng_t* rng, u32 percent)
{
	return (rng_next(rng) >> 33) % 100 < percent;
}
