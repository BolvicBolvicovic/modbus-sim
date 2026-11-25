#ifndef FAULT_H
#define FAULT_H

#include "vlib/c_types.h"
#include "util/rng.h"
#include <stdbool.h>

#define FAULT_MAX_SCHEDULE_STEPS 8

/* Name: fault_kind_t
 * Description: one independently configurable fault on a device.
 * - FAULT_DELAY             : the response is sent `delay_ms` late
 * - FAULT_DROP              : the response is never sent
 * - FAULT_EXCEPTION         : the response is replaced by an exception PDU
 * - FAULT_CORRUPT           : the response is corrupted as `corrupt_mode` says
 * - FAULT_DISCONNECT        : the response is dropped and the connection is closed
 * - FAULT_RESTART           : the device restarts after the request is processed
 * - FAULT_SENSOR_DISCONNECT : the device reports its sensors as disconnected
 * Keep the existing order: kinds are appended so fault bits and the RNG draw
 * order of already configured faults stay stable.
 * */
typedef enum fault_kind_t {
	FAULT_DELAY,
	FAULT_DROP,
	FAULT_EXCEPTION,
	FAULT_CORRUPT,
	FAULT_DISCONNECT,
	FAULT_RESTART,
	FAULT_SENSOR_DISCONNECT,
	FAULT_KIND_COUNT,
} fault_kind_t;

#define FAULT_BIT(kind) ((u8)(1u << (kind)))
_Static_assert(FAULT_KIND_COUNT <= 8, "fault mask no longer fits in u8");

/* Name: fault_corrupt_mode_t
 * Description: how a fired FAULT_CORRUPT damages the response.
 * - FAULT_CORRUPT_NONE     : not corrupting; in a fault_t it means FAULT_CORRUPT_PDU
 * - FAULT_CORRUPT_PDU      : flip one byte of the response PDU
 * - FAULT_CORRUPT_REGISTER : flip one register value before it is encoded
 * - FAULT_CORRUPT_CHECKSUM : damage the transport checksum
 * */
typedef enum fault_corrupt_mode_t {
	FAULT_CORRUPT_NONE = 0,
	FAULT_CORRUPT_PDU,
	FAULT_CORRUPT_REGISTER,
	FAULT_CORRUPT_CHECKSUM,
} fault_corrupt_mode_t;

/* Name: fault_schedule_step_t
 * Description: one step of a deterministic request schedule. A step fires for
 * exactly `count` consecutive requests after waiting `after_ms`.
 * */
typedef struct fault_schedule_step_t fault_schedule_step_t;
struct fault_schedule_step_t
{
	u32	after_ms;
	u32	count;
};

typedef struct fault_t fault_t;
struct fault_t
{
	u32			delay_ms;
	u8			exception_code;	/* standard Modbus exception code, nonzero */
	u32			percent;	/* chance per request, 0..100 */
	fault_corrupt_mode_t	corrupt_mode;	/* used only by FAULT_CORRUPT */
	u8			schedule_count;	/* 0..FAULT_MAX_SCHEDULE_STEPS */
	u64			schedule_id;	/* 0 = no schedule */
	u32			source;		/* 0 = device base, else machine/state overlay; set by device_effective_faults */
	u64			origin_ms;	/* overlay activation time, the schedule origin when source != 0 */
	fault_schedule_step_t	schedule[FAULT_MAX_SCHEDULE_STEPS];
};

/* Name: fault_decision_t
 * Description: what the server must do with one response.
 * A nonzero exception_code replaces the response; corrupt selects the damage.
 * restart and sensor_disconnect are actions and do not drop the response.
 * */
typedef struct fault_decision_t fault_decision_t;
struct fault_decision_t
{
	bool			drop;
	bool			disconnect;
	bool			restart;
	bool			sensor_disconnect;
	u32			delay_ms;
	u8			exception_code;
	fault_corrupt_mode_t	corrupt;
};

typedef struct fault_set_t fault_set_t;
struct fault_set_t
{
	u8		present;
	fault_t		by_kind[FAULT_KIND_COUNT];
};

/* Name: fault_schedule_state_t
 * Description: progress through one kind's schedule across requests.
 * `step` is the next step to start and equals schedule_count when the schedule
 * has finished.
 * */
typedef struct fault_schedule_state_t fault_schedule_state_t;
struct fault_schedule_state_t
{
	u64	schedule_id;	/* schedule the progress below belongs to */
	u32	source;		/* fault source and activation time the progress belongs to */
	u64	origin_ms;
	u16	step;
	u32	remaining;	/* requests left in the active step */
	u64	next_at_ms;	/* earliest start time for `step` */
};

typedef struct fault_runtime_t fault_runtime_t;
struct fault_runtime_t
{
	fault_schedule_state_t	by_kind[FAULT_KIND_COUNT];
};

/* Rolls each configured fault independently and combines the decisions for one
 * response. `runtime` carries per-kind schedule progress across requests;
 * `start_ms` is the schedule origin (device start or last restart) of base
 * faults; an overlay fault (source != 0) uses its own origin_ms instead. A fault
 * with a nonzero schedule_id fires by schedule instead of percent: the first step
 * starts at the origin + after_ms, each step fires for count consecutive
 * requests, and each later step starts after_ms after the request that completed
 * the previous one. Changing schedule_id, source or origin_ms, or removing the kind from the active
 * set, resets that kind's progress. An empty
 * set never drops, delays or disconnects. A fired corrupt fault with
 * FAULT_CORRUPT_NONE in the set still reports FAULT_CORRUPT_PDU. */
fault_decision_t	fault_decide_at(const fault_set_t* set, rng_t* rng,
					fault_runtime_t* runtime, u64 now_ms, u64 start_ms);

#endif
