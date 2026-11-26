/**
 * @file
 * @brief Monotonic millisecond clock.
 */
#ifndef CLOCK_H
#define CLOCK_H

#include "vlib/c_types.h"

/**
 * @brief Monotonic time in milliseconds.
 *
 * Never goes backwards.
 */
u64	clock_now_ms(void);

/**
 * @brief Monotonic time in microseconds.
 *
 * Never goes backwards.
 */
u64	clock_now_us(void);

#endif
