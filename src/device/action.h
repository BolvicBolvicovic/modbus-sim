/**
 * @file
 * @brief Point actions: statements attached to a point that run when a Modbus client reads or
 *        writes it.
 */
#ifndef ACTION_H
#define ACTION_H

#include "device/machine.h"

typedef enum action_event_t action_event_t;
typedef struct action_instance_t action_instance_t;
typedef struct action_system_t action_system_t;

#define ACTION_MAX_INSTANCES	256

/** @brief When a point action runs. */
enum action_event_t
{
	ACTION_ON_READ,
	ACTION_ON_WRITE,
};

/**
 * @brief One compiled point action attached to one point.
 *
 * Variables are the parameters followed by the `(var ...)` variables; `defs` holds their names
 * and configured initial values, `vars` the current ones. Statements are the machine
 * statements: the top-level ones come first, nested bodies after them.
 */
struct action_instance_t
{
	space_kind_t	space;
	u32		address;		/**< first address of the attached point */
	action_event_t	event;
	char		name[MACHINE_NAME_MAX];	/**< the define-action name; empty for an inline body */
	u16		param_count;		/**< the first `param_count` variables are parameters */
	u16		var_count;		/**< parameters plus variables */
	u16		top_count;		/**< top-level statements, starting at index 0 */
	u16		statement_count;
	u16		node_count;
	machine_stmt_t*	statements;
	expr_node_t*	nodes;
	machine_var_def_t* defs;
	val_t*		vars;
	u64		last_ms;		/**< request time of the last run */
	bool		ran;
	u8		warned;			/**< bitmask of expr_failure_t already logged */
};

/** @brief The point actions of one device, sorted by space, address and event. */
struct action_system_t
{
	u16		count;
	action_instance_t items[ACTION_MAX_INSTANCES];
};

/**
 * @brief Adds a point action, copying the compiled body of `body`.
 *
 * `body` holds one state whose on-tick statements are the action; its first `param_count`
 * variables are the parameters. Returns NULL on success or a static error text.
 */
const char*	device_add_action(device_t* dev, space_kind_t space, u32 address,
				  action_event_t event, const char* name, u16 param_count,
				  const machine_def_t* body);

/**
 * @brief Checks the statements of every action once machines own their points.
 *
 * A set! target must not be driven by a machine. Returns NULL or a static error text.
 */
const char*	device_finalize_actions(const device_t* dev);

/**
 * @brief Runs the on-read actions of the points overlapping [address, address + count), in
 *        address order, once each.
 *
 * Returns false when one failed (logged once per instance and failure kind); the caller
 * answers exception 04. Host reads never call this.
 */
bool		device_actions_on_read(device_t* dev, space_kind_t space, u32 address, u32 count);

/** @brief device_actions_on_read() for the on-write actions, after a client write was applied. */
bool		device_actions_on_write(device_t* dev, space_kind_t space, u32 address, u32 count);

/** @brief Resets variables to their initial values, keeping the current parameters. */
void		device_actions_restart(device_t* dev);

/** @brief Instance number `index` in sorted order, or NULL. */
const action_instance_t* device_action_at(const device_t* dev, u32 index);

/**
 * @brief Reads the current value of parameter `name` of the action `event` attached to the
 *        point at `space`/`address`.
 */
bool		device_action_param_get(const device_t* dev, space_kind_t space, u32 address,
					action_event_t event, const char* name, val_t* out);

/** @brief Sets a parameter; the value must have the parameter's type. */
bool		device_action_param_set(device_t* dev, space_kind_t space, u32 address,
					action_event_t event, const char* name, const val_t* in);

#endif
