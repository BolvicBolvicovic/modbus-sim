#ifndef FAULT_H
#define FAULT_H

#include "vlib/c_types.h"
#include "util/rng.h"
#include <stdbool.h>

/* Name: fault_kind_t
 * Description: one independently configurable fault on a device.
 * - FAULT_DELAY      : the response is sent `delay_ms` late
 * - FAULT_DROP       : the response is never sent
 * - FAULT_EXCEPTION  : the response is replaced by an exception PDU
 * - FAULT_CORRUPT    : the response payload is flipped
 * - FAULT_DISCONNECT : the response is dropped and the connection is closed
 * */
typedef enum fault_kind_t {
	FAULT_DELAY,
	FAULT_DROP,
	FAULT_EXCEPTION,
	FAULT_CORRUPT,
	FAULT_DISCONNECT,
	FAULT_KIND_COUNT,
} fault_kind_t;

#define FAULT_BIT(kind) ((u8)(1u << (kind)))
_Static_assert(FAULT_KIND_COUNT <= 8, "fault mask no longer fits in u8");

typedef struct fault_t fault_t;
struct fault_t
{
	u32		delay_ms;
	u8		exception_code;	/* standard Modbus exception code, nonzero */
	u32		percent;	/* chance per request, 0..100 */
};

/* Name: fault_decision_t
 * Description: what the server must do with one response.
 * A nonzero exception_code replaces the response; corrupt flips its payload byte.
 * */
typedef struct fault_decision_t fault_decision_t;
struct fault_decision_t
{
	bool	drop;
	bool	disconnect;
	u32	delay_ms;
	u8	exception_code;
	bool	corrupt;
};

typedef struct fault_set_t fault_set_t;
struct fault_set_t
{
	u8		present;
	fault_t		by_kind[FAULT_KIND_COUNT];
};

/* Rolls each configured fault independently and combines the decisions for one
 * response. An empty set never drops, delays or disconnects. */
fault_decision_t	fault_decide(const fault_set_t* set, rng_t* rng);

#endif
