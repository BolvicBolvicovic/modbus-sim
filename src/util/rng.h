#ifndef RNG_H
#define RNG_H

#include "vlib/c_types.h"
#include <stdbool.h>

/* Name: rng_t
 * Description: tiny deterministic xorshift64* generator.
 * Same seed, same sequence: fault injection is reproducible.
 * */
typedef struct rng_t rng_t;
struct rng_t
{
	u64	state;
};

void	rng_seed(rng_t* rng, u64 seed);
u64	rng_next(rng_t* rng);

/* Returns true with a probability of `percent` (0..100). */
bool	rng_percent(rng_t* rng, u32 percent);

#endif
