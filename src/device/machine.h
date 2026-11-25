#ifndef MACHINE_H
#define MACHINE_H

#include "device/device.h"
#include "device/expr.h"
#include "util/rng.h"
#include <stdbool.h>

#define MACHINE_MAX_MACHINES      8
#define MACHINE_MAX_STATES        16
#define MACHINE_MAX_TRANSITIONS   32
#define MACHINE_MAX_VARS          16
#define MACHINE_MAX_STATEMENTS    128
#define MACHINE_MAX_EXPR_NODES    512
#define MACHINE_MAX_FIRINGS       64
#define MACHINE_EVAL_BUDGET       1000
#define MACHINE_NAME_MAX          64

/* Machine runtime descriptions. T4 fills these while reading the config; the
 * runtime deep-copies them into the device arena and never touches s7. */
typedef enum machine_stmt_kind_t machine_stmt_kind_t;
enum machine_stmt_kind_t
{
	STMT_SET, STMT_WHEN, STMT_UNLESS,
};

typedef enum machine_target_kind_t machine_target_kind_t;
enum machine_target_kind_t
{
	TARGET_VAR, TARGET_POINT,
};

typedef enum machine_target_status_t machine_target_status_t;
enum machine_target_status_t
{
	MACHINE_TARGET_OK,
	MACHINE_TARGET_NOT_FOUND,
	MACHINE_TARGET_RAMP,
	MACHINE_TARGET_CONFLICT,
};

typedef struct machine_target_result_t machine_target_result_t;
struct machine_target_result_t
{
	u32			index;
	machine_target_status_t	status;
};

typedef enum machine_after_kind_t machine_after_kind_t;
enum machine_after_kind_t
{
	AFTER_NONE, AFTER_LITERAL, AFTER_RANDOM,
};

typedef struct machine_var_def_t machine_var_def_t;
struct machine_var_def_t
{
	char	name[MACHINE_NAME_MAX];
	val_t	initial;
};

typedef struct machine_stmt_t machine_stmt_t;
struct machine_stmt_t
{
	machine_stmt_kind_t kind;
	u16	expr_root;		/* STMT_SET: value expr; STMT_WHEN/UNLESS: condition */
	machine_target_kind_t target_kind; /* STMT_SET only */
	u16	var_index;		/* TARGET_VAR */
	u8	space;			/* TARGET_POINT: numeric space_kind_t value */
	u32	address;		/* TARGET_POINT */
	u16	first_child;		/* STMT_WHEN/UNLESS: first child statement */
	u16	child_count;		/* number of child statements */
};

typedef struct machine_state_def_t machine_state_def_t;
struct machine_state_def_t
{
	char	name[MACHINE_NAME_MAX];
	u16	on_enter_first, on_enter_count;
	u16	on_tick_first,  on_tick_count;
	u16	on_exit_first,  on_exit_count;
};

typedef struct machine_transition_def_t machine_transition_def_t;
struct machine_transition_def_t
{
	u16	from, to;
	bool	has_when;
	u16	when_root;
	machine_after_kind_t after_kind;
	u64	after_literal_ms;
	u64	after_min_ms, after_max_ms;
};

typedef struct machine_def_t machine_def_t;
struct machine_def_t
{
	char	name[MACHINE_NAME_MAX];
	u16	initial_state;
	u16	var_count;
	machine_var_def_t vars[MACHINE_MAX_VARS];
	u16	state_count;
	machine_state_def_t states[MACHINE_MAX_STATES];
	u16	transition_count;
	machine_transition_def_t transitions[MACHINE_MAX_TRANSITIONS];
	u16	statement_count;
	machine_stmt_t statements[MACHINE_MAX_STATEMENTS];
	u16	node_count;
	expr_node_t nodes[MACHINE_MAX_EXPR_NODES];
};

/* Declared in device.h; the fields stay private to the runtime. */
typedef struct machine_system_t machine_system_t;
struct machine_system_t
{
	machine_def_t	defs[MACHINE_MAX_MACHINES];
	u16		count;
	bool		finalized;
	u64		seed;			/* raw server seed, mixed with unit id on finalize */
	rng_t		rng;

	u16		current[MACHINE_MAX_MACHINES];
	u64		entered_at[MACHINE_MAX_MACHINES];
	u64		advanced_to[MACHINE_MAX_MACHINES];
	val_t		vars[MACHINE_MAX_MACHINES][MACHINE_MAX_VARS];
	u64		deadline[MACHINE_MAX_MACHINES][MACHINE_MAX_TRANSITIONS];
	/* [machine][state][handler] bitmask of failure kinds already warned about. */
	u8		warned[MACHINE_MAX_MACHINES][MACHINE_MAX_STATES][4];
	bool		cap_warned[MACHINE_MAX_MACHINES];
};

/* Checks one set! point target, rejecting ramps and points already claimed by
 * another machine. On success, records `machine_id` as the point's owner. */
machine_target_result_t machine_check_target_point(const device_t* dev, space_kind_t space,
						   u32 address, u8* owners, u8 machine_id);

/* Adds a machine copy to `dev`. Returns NULL on success or a static error text. */
const char*	device_add_machine(device_t* dev, const machine_def_t* def);

/* Validates and initializes all added machines at dev->start_ms. */
const char*	device_finalize_machines(device_t* dev);

/* Call before finalize. Mixes `server_seed` with the device unit id. */
void		device_seed_machines(device_t* dev, u64 server_seed);

/* Advances every machine in declaration order. Called by device_tick. */
void		device_machines_advance(device_t* dev, u64 now_ms);

#endif
