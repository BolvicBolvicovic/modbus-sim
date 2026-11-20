#ifndef FAULT_H
#define FAULT_H

#include "vlib/c_types.h"
#include "util/rng.h"
#include <stdbool.h>

/* Name: fault_kind_t
 * Description: the one fault a device can have.
 * - FAULT_DELAY: the response is sent `delay_ms` late
 * - FAULT_DROP : the response is never sent
 * */
typedef enum fault_kind_t fault_kind_t;
enum fault_kind_t
{
	FAULT_NONE,
	FAULT_DELAY,
	FAULT_DROP,
};

typedef struct fault_t fault_t;
struct fault_t
{
	fault_kind_t	kind;
	u32		delay_ms;
	u32		percent;	/* chance per request, 0..100 */
};

/* Name: fault_decision_t
 * Description: what the server must do with one response.
 * */
typedef struct fault_decision_t fault_decision_t;
struct fault_decision_t
{
	bool	drop;
	u32	delay_ms;
};

/* Rolls the dice for one response. A device without a fault never drops or delays. */
fault_decision_t	fault_decide(const fault_t* fault, rng_t* rng);

#endif
