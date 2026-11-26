/**
 * @file
 * @brief Seeded pseudo-random numbers for reproducible faults.
 */
#ifndef RNG_H
#define RNG_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct rng_t rng_t;

/**
 * @brief Deterministic xoshiro256** generator seeded via splitmix64.
 *
 * Same seed, same sequence: fault injection is reproducible.
 */
struct rng_t
{
	u64	s[4];
};

void	rng_seed(rng_t* rng, u64 seed);
u64	rng_next(rng_t* rng);

/** @brief Returns true with a probability of `percent` (0..100). */
bool	rng_percent(rng_t* rng, u32 percent);

#endif
