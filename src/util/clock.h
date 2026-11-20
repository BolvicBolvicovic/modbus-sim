#ifndef CLOCK_H
#define CLOCK_H

#include "vlib/c_types.h"

/* Name: clock_now_ms
 * Description: monotonic time in milliseconds. Never goes backwards.
 * */
u64	clock_now_ms(void);

#endif
