#include "device/machine.h"
#include "util/log.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MACHINE_AFTER_MAX_MS	86400000ULL
#define MACHINE_SEED_FACTOR	0x9E3779B97F4A7C15ULL
#define MACHINE_FAULT_MASK	((1u << FAULT_KIND_COUNT) - 1)

static const char* const handler_names[MACHINE_ROLE_COUNT] = {
	"on-enter", "on-tick", "on-exit", "transition condition", "on-write",
};

static const char* const failure_names[EXPR_FAIL_TYPE + 1] = {
	"", "evaluation budget exhausted", "division by zero",
	"point access failed", "invalid expression",
};

static char error_text[256];

static const char*
machine_error(const char* machine, const char* state, const char* reason)
{
	if (machine && state)
		snprintf(error_text, sizeof(error_text), "machine '%s', state '%s': %s",
			 machine, state, reason);
	else if (machine)
		snprintf(error_text, sizeof(error_text), "machine '%s': %s", machine, reason);
	else
		snprintf(error_text, sizeof(error_text), "%s", reason);
	return error_text;
}

static bool
name_ok(const char* name)
{
	return name[0] != '\0' && memchr(name, '\0', MACHINE_NAME_MAX) != NULL;
}

machine_target_result_t
machine_check_target_point(const device_t* dev, space_kind_t space, u32 address,
			   u8* owners, u8 machine_id)
{
	machine_target_result_t result = {
		.index = device_find_point(dev, space, address),
		.status = MACHINE_TARGET_OK,
	};

	if (result.index == DEVICE_MAX_POINTS)
	{
		result.status = MACHINE_TARGET_NOT_FOUND;
		return result;
	}
	if (dev->points[result.index].kind == POINT_RAMP)
	{
		result.status = MACHINE_TARGET_RAMP;
		return result;
	}
	if (owners[result.index] != 0 && owners[result.index] != machine_id)
	{
		result.status = MACHINE_TARGET_CONFLICT;
		return result;
	}
	owners[result.index] = machine_id;
	return result;
}

typedef struct validation_t validation_t;
struct validation_t
{
	device_t*		dev;
	machine_system_t*	sys;
	u16		machine_index;
	u8		expr_marks[MACHINE_MAX_EXPR_NODES];
	val_type_t	expr_types[MACHINE_MAX_EXPR_NODES];
	u8		stmt_marks[MACHINE_MAX_STATEMENTS];
	u8		point_owner[DEVICE_MAX_POINTS]; /* machine index + 1, from set! */
	u8		reg_owner[DEVICE_MAX_POINTS];   /* machine index + 1, from state registers */
};

static const char*
validate_expr(const machine_def_t* def, u16 root, const char* machine, const char* state,
	      validation_t* v, val_type_t* out)
{
	const expr_node_t* node = NULL;
	val_type_t lhs = VAL_NUM;
	val_type_t rhs = VAL_NUM;
	const char* error = NULL;

	if (root >= def->node_count)
		return machine_error(machine, state, "expression node index out of range");
	if (v->expr_marks[root] == 1)
		return machine_error(machine, state, "expression node cycle");
	if (v->expr_marks[root] == 2)
	{
		*out = v->expr_types[root];
		return NULL;
	}
	v->expr_marks[root] = 1;
	node = &def->nodes[root];

	switch (node->kind)
	{
	case EXPR_BOOL:
		if (node->literal.type != VAL_BOOL)
			return machine_error(machine, state, "boolean literal has the wrong value type");
		*out = VAL_BOOL;
		break;
	case EXPR_NUM:
		if (node->literal.type != VAL_NUM)
			return machine_error(machine, state, "number literal has the wrong value type");
		*out = VAL_NUM;
		break;
	case EXPR_VAR:
		if (node->a == EXPR_NO_CHILD || node->a >= def->var_count)
			return machine_error(machine, state, "expression variable index out of range");
		*out = def->vars[node->a].initial.type;
		break;
	case EXPR_POINT:
		if (node->space >= SPACE_COUNT || node->address >= ADDRESS_SPACE_SIZE)
			return machine_error(machine, state, "expression point address is out of range");
		*out = space_is_bit((space_kind_t)node->space) ? VAL_BOOL : VAL_NUM;
		break;
	case EXPR_NOW_MS:
	case EXPR_ELAPSED_MS:
		*out = VAL_NUM;
		break;
	case EXPR_ADD:
	case EXPR_SUB:
	case EXPR_MUL:
	case EXPR_DIV:
	case EXPR_MIN:
	case EXPR_MAX:
		if (node->a == EXPR_NO_CHILD || node->a >= def->node_count ||
		    node->b == EXPR_NO_CHILD || node->b >= def->node_count)
			return machine_error(machine, state, "expression child index out of range");
		error = validate_expr(def, node->a, machine, state, v, &lhs);
		if (error)
			return error;
		error = validate_expr(def, node->b, machine, state, v, &rhs);
		if (error)
			return error;
		if (lhs != VAL_NUM || rhs != VAL_NUM)
			return machine_error(machine, state, "arithmetic operands must be numbers");
		*out = VAL_NUM;
		break;
	case EXPR_LT:
	case EXPR_LE:
	case EXPR_GT:
	case EXPR_GE:
		if (node->a == EXPR_NO_CHILD || node->a >= def->node_count ||
		    node->b == EXPR_NO_CHILD || node->b >= def->node_count)
			return machine_error(machine, state, "expression child index out of range");
		error = validate_expr(def, node->a, machine, state, v, &lhs);
		if (error)
			return error;
		error = validate_expr(def, node->b, machine, state, v, &rhs);
		if (error)
			return error;
		if (lhs != VAL_NUM || rhs != VAL_NUM)
			return machine_error(machine, state, "comparison operands must be numbers");
		*out = VAL_BOOL;
		break;
	case EXPR_EQ:
		if (node->a == EXPR_NO_CHILD || node->a >= def->node_count ||
		    node->b == EXPR_NO_CHILD || node->b >= def->node_count)
			return machine_error(machine, state, "expression child index out of range");
		error = validate_expr(def, node->a, machine, state, v, &lhs);
		if (error)
			return error;
		error = validate_expr(def, node->b, machine, state, v, &rhs);
		if (error)
			return error;
		if (lhs != rhs)
			return machine_error(machine, state, "equality operands must have the same type");
		*out = VAL_BOOL;
		break;
	case EXPR_AND:
	case EXPR_OR:
		if (node->a == EXPR_NO_CHILD || node->a >= def->node_count ||
		    node->b == EXPR_NO_CHILD || node->b >= def->node_count)
			return machine_error(machine, state, "expression child index out of range");
		error = validate_expr(def, node->a, machine, state, v, &lhs);
		if (error)
			return error;
		error = validate_expr(def, node->b, machine, state, v, &rhs);
		if (error)
			return error;
		if (lhs != VAL_BOOL || rhs != VAL_BOOL)
			return machine_error(machine, state, "logical operands must be booleans");
		*out = VAL_BOOL;
		break;
	case EXPR_NOT:
		if (node->a == EXPR_NO_CHILD || node->a >= def->node_count)
			return machine_error(machine, state, "expression child index out of range");
		error = validate_expr(def, node->a, machine, state, v, &lhs);
		if (error)
			return error;
		if (lhs != VAL_BOOL)
			return machine_error(machine, state, "NOT operand must be a boolean");
		*out = VAL_BOOL;
		break;
	case EXPR_IF:
		if (node->a == EXPR_NO_CHILD || node->a >= def->node_count ||
		    node->b == EXPR_NO_CHILD || node->b >= def->node_count ||
		    node->c == EXPR_NO_CHILD || node->c >= def->node_count)
			return machine_error(machine, state, "expression child index out of range");
		error = validate_expr(def, node->a, machine, state, v, &lhs);
		if (error)
			return error;
		error = validate_expr(def, node->b, machine, state, v, &rhs);
		if (error)
			return error;
		error = validate_expr(def, node->c, machine, state, v, out);
		if (error)
			return error;
		if (lhs != VAL_BOOL)
			return machine_error(machine, state, "IF condition must be a boolean");
		if (rhs != *out)
			return machine_error(machine, state, "IF branches must have the same type");
		break;
	default:
		return machine_error(machine, state, "unknown expression kind");
	}

	v->expr_marks[root] = 2;
	v->expr_types[root]  = *out;
	return NULL;
}

static const char*
validate_statement(const machine_def_t* def, u16 index, const char* machine, const char* state,
		   validation_t* v)
{
	const machine_stmt_t* stmt = NULL;
	machine_target_result_t target = { 0 };
	val_type_t value_type = VAL_NUM;
	val_type_t target_type = VAL_NUM;
	u32 point_index = DEVICE_MAX_POINTS;
	const char* error = NULL;

	if (index >= def->statement_count)
		return machine_error(machine, state, "statement index out of range");
	if (v->stmt_marks[index] == 1)
		return machine_error(machine, state, "statement cycle");
	if (v->stmt_marks[index] == 2)
		return NULL;
	v->stmt_marks[index] = 1;
	stmt = &def->statements[index];

	switch (stmt->kind)
	{
	case STMT_SET:
		error = validate_expr(def, stmt->expr_root, machine, state, v, &value_type);
		if (error)
			return error;
		if (stmt->target_kind == TARGET_VAR)
		{
			if (stmt->var_index >= def->var_count)
				return machine_error(machine, state, "assignment variable index out of range");
			target_type = def->vars[stmt->var_index].initial.type;
		}
		else if (stmt->target_kind == TARGET_POINT)
		{
			point_index = device_find_point(v->dev, (space_kind_t)stmt->space, stmt->address);
			if (point_index != DEVICE_MAX_POINTS && v->reg_owner[point_index] != 0)
				return machine_error(machine, state,
						     "state register cannot be driven by a machine");
			target = machine_check_target_point(v->dev, (space_kind_t)stmt->space,
							    stmt->address, v->point_owner,
							    (u8)(v->machine_index + 1));
			if (target.status == MACHINE_TARGET_NOT_FOUND)
				return machine_error(machine, state,
						     "assignment target must be the first address of a configured point");
			if (target.status == MACHINE_TARGET_RAMP)
				return machine_error(machine, state, "machine cannot drive a ramp point");
			if (target.status == MACHINE_TARGET_CONFLICT)
				return machine_error(machine, state,
						     "point is already driven by another machine");
			target_type = space_is_bit((space_kind_t)stmt->space) ? VAL_BOOL : VAL_NUM;
		}
		else
			return machine_error(machine, state, "unknown assignment target kind");

		if (value_type != target_type)
			return machine_error(machine, state, "assignment value has the wrong type");
		break;
	case STMT_WHEN:
	case STMT_UNLESS:
		error = validate_expr(def, stmt->expr_root, machine, state, v, &value_type);
		if (error)
			return error;
		if (value_type != VAL_BOOL)
			return machine_error(machine, state, "condition must be a boolean");
		if ((u32)stmt->first_child + stmt->child_count > def->statement_count)
			return machine_error(machine, state, "statement child range out of range");
		for (u32 i = 0; i < stmt->child_count; i++)
		{
			error = validate_statement(def, (u16)(stmt->first_child + i), machine, state, v);
			if (error)
				return error;
		}
		break;
	default:
		return machine_error(machine, state, "unknown statement kind");
	}

	v->stmt_marks[index] = 2;
	return NULL;
}

static const char*
validate_machine(validation_t* v, u16 machine_index)
{
	const machine_def_t* def = &v->sys->defs[machine_index];
	const char* machine = def->name;
	const char* error = NULL;
	val_type_t condition_type = VAL_NUM;
	u16 first = 0;
	u16 count = 0;

	memset(v->expr_marks, 0, sizeof(v->expr_marks));
	v->machine_index = machine_index;
	if (!name_ok(machine))
		return machine_error(NULL, NULL, "machine name is empty or not null-terminated");
	for (u16 i = 0; i < machine_index; i++)
	{
		if (strcmp(v->sys->defs[i].name, machine) == 0)
			return machine_error(machine, NULL, "duplicate machine name");
	}

	if (def->var_count > MACHINE_MAX_VARS)
		return machine_error(machine, NULL, "too many variables in one machine");
	if (def->state_count == 0)
		return machine_error(machine, NULL, "machine has no states");
	if (def->state_count > MACHINE_MAX_STATES)
		return machine_error(machine, NULL, "too many states in one machine");
	if (def->transition_count > MACHINE_MAX_TRANSITIONS)
		return machine_error(machine, NULL, "too many transitions in one machine");
	if (def->statement_count > MACHINE_MAX_STATEMENTS)
		return machine_error(machine, NULL, "too many statements in one machine");
	if (def->write_handler_count > MACHINE_MAX_WRITE_HANDLERS)
		return machine_error(machine, NULL, "too many on-write handlers in one machine");
	if (def->node_count > MACHINE_MAX_EXPR_NODES)
		return machine_error(machine, NULL, "too many expression nodes in one machine");

	for (u16 i = 0; i < def->var_count; i++)
	{
		const machine_var_def_t* var = &def->vars[i];

		if (!name_ok(var->name))
			return machine_error(machine, NULL, "variable name is empty or not null-terminated");
		for (u16 j = 0; j < i; j++)
		{
			if (strcmp(def->vars[j].name, var->name) == 0)
				return machine_error(machine, NULL, "duplicate variable name");
		}
		if (var->initial.type != VAL_BOOL && var->initial.type != VAL_NUM)
			return machine_error(machine, NULL, "variable initial value has an invalid type");
		if (var->initial.type == VAL_NUM && !isfinite(var->initial.n))
			return machine_error(machine, NULL, "variable initial value must be finite");
	}

	for (u16 i = 0; i < def->state_count; i++)
	{
		const machine_state_def_t* state = &def->states[i];
		const char* state_name = state->name;

		if (!name_ok(state_name))
			return machine_error(machine, NULL, "state name is empty or not null-terminated");
		if ((state->faults.present & ~MACHINE_FAULT_MASK) != 0)
			return machine_error(machine, state_name, "state fault overlay has an unknown kind");
		for (u16 j = 0; j < i; j++)
		{
			if (strcmp(def->states[j].name, state_name) == 0)
				return machine_error(machine, state_name, "duplicate state name");
		}
	}

	if (def->initial_state >= def->state_count)
		return machine_error(machine, NULL, "initial state index out of range");

	for (u16 i = 0; i < def->transition_count; i++)
	{
		const machine_transition_def_t* t = &def->transitions[i];
		const char* state_name = def->states[t->from < def->state_count ? t->from : 0].name;

		if (t->from >= def->state_count || t->to >= def->state_count)
			return machine_error(machine, NULL, "transition state index out of range");
		if (!t->has_when && t->after_kind == AFTER_NONE)
			return machine_error(machine, state_name, "transition has no trigger");
		if (t->after_kind == AFTER_LITERAL)
		{
			if (t->after_literal_ms == 0 || t->after_literal_ms > MACHINE_AFTER_MAX_MS)
				return machine_error(machine, state_name,
						     "literal after-ms must be between 1 and 86400000");
		}
		else if (t->after_kind == AFTER_RANDOM)
		{
			if (t->after_min_ms == 0 ||
			    t->after_max_ms > MACHINE_AFTER_MAX_MS ||
			    t->after_min_ms > t->after_max_ms)
				return machine_error(machine, state_name,
						     "random after-ms bounds must be between 1 and 86400000 with min <= max");
		}
		else if (t->after_kind != AFTER_NONE)
			return machine_error(machine, state_name, "unknown after-ms kind");

		if (t->has_when)
		{
			error = validate_expr(def, t->when_root, machine, state_name, v, &condition_type);
			if (error)
				return error;
			if (condition_type != VAL_BOOL)
				return machine_error(machine, state_name, "transition condition must be a boolean");
		}
	}

	for (u16 state = 0; state < def->state_count; state++)
	{
		const machine_state_def_t* s = &def->states[state];
		const char* state_name = s->name;

		first = s->on_enter_first;
		count = s->on_enter_count;
		if ((u32)first + count > def->statement_count)
			return machine_error(machine, state_name, "on-enter statement range out of range");
		if (count > 0)
			memset(v->stmt_marks, 0, sizeof(v->stmt_marks));
		for (u32 i = 0; i < count; i++)
		{
			error = validate_statement(def, (u16)(first + i), machine, state_name, v);
			if (error)
				return error;
		}

		first = s->on_tick_first;
		count = s->on_tick_count;
		if ((u32)first + count > def->statement_count)
			return machine_error(machine, state_name, "on-tick statement range out of range");
		if (count > 0)
			memset(v->stmt_marks, 0, sizeof(v->stmt_marks));
		for (u32 i = 0; i < count; i++)
		{
			error = validate_statement(def, (u16)(first + i), machine, state_name, v);
			if (error)
				return error;
		}

		first = s->on_exit_first;
		count = s->on_exit_count;
		if ((u32)first + count > def->statement_count)
			return machine_error(machine, state_name, "on-exit statement range out of range");
		if (count > 0)
			memset(v->stmt_marks, 0, sizeof(v->stmt_marks));
		for (u32 i = 0; i < count; i++)
		{
			error = validate_statement(def, (u16)(first + i), machine, state_name, v);
			if (error)
				return error;
		}
	}

	for (u16 w = 0; w < def->write_handler_count; w++)
	{
		const machine_write_handler_t* h = &def->write_handlers[w];

		first = h->first;
		count = h->count;
		if ((u32)first + count > def->statement_count)
			return machine_error(machine, NULL, "on-write statement range out of range");
		if (count > 0)
			memset(v->stmt_marks, 0, sizeof(v->stmt_marks));
		for (u32 i = 0; i < count; i++)
		{
			error = validate_statement(def, (u16)(first + i), machine, NULL, v);
			if (error)
				return error;
		}
	}
	return NULL;
}

static const char*
collect_state_registers(validation_t* v)
{
	for (u16 m = 0; m < v->sys->count; m++)
	{
		const machine_def_t* def = &v->sys->defs[m];
		u32 index = 0;

		if (!def->has_state_register)
			continue;
		if (def->state_register_space != SPACE_HOLDING &&
		    def->state_register_space != SPACE_INPUT)
			return machine_error(def->name, NULL,
					     "state register must be a holding or input point");
		index = device_find_point(v->dev, (space_kind_t)def->state_register_space,
					  def->state_register_address);
		if (index == DEVICE_MAX_POINTS)
			return machine_error(def->name, NULL,
					     "state register must name the first address of a configured point");
		if (v->dev->points[index].kind == POINT_RAMP)
			return machine_error(def->name, NULL, "state register cannot name a ramp point");
		if (v->reg_owner[index] != 0)
			return machine_error(def->name, NULL,
					     "state register point is also used by another machine");
		v->reg_owner[index] = (u8)(m + 1);
	}
	return NULL;
}

static const char*
validate_write_handlers(validation_t* v)
{
	for (u16 m = 0; m < v->sys->count; m++)
	{
		const machine_def_t* def = &v->sys->defs[m];

		for (u16 w = 0; w < def->write_handler_count; w++)
		{
			const machine_write_handler_t* h = &def->write_handlers[w];
			u32 index = device_find_point(v->dev, (space_kind_t)h->space, h->address);

			if ((u32)h->first + h->count > def->statement_count)
				return machine_error(def->name, NULL, "on-write statement range out of range");
			if (index == DEVICE_MAX_POINTS)
				return machine_error(def->name, NULL,
						     "on-write target must name the first address of a configured point");
			if (v->point_owner[index] != 0 ||
			    v->reg_owner[index] != 0 ||
			    v->dev->points[index].kind == POINT_MACHINE)
				return machine_error(def->name, NULL,
						     "on-write cannot target a machine-driven point");
		}
	}
	return NULL;
}

static void
mark_point_owner(device_t* dev, const machine_def_t* def, u16 index, u8* seen)
{
	const machine_stmt_t* stmt = NULL;
	u32 point = 0;

	if (index >= def->statement_count || seen[index])
		return;
	seen[index] = 1;
	stmt = &def->statements[index];
	if (stmt->kind == STMT_SET && stmt->target_kind == TARGET_POINT)
	{
		point = device_find_point(dev, (space_kind_t)stmt->space, stmt->address);
		if (point != DEVICE_MAX_POINTS)
			dev->points[point].kind = POINT_MACHINE;
	}
	else if (stmt->kind == STMT_WHEN || stmt->kind == STMT_UNLESS)
	{
		for (u32 i = 0; i < stmt->child_count; i++)
			mark_point_owner(dev, def, (u16)(stmt->first_child + i), seen);
	}
}

static void
mark_point_owners(device_t* dev, machine_system_t* sys)
{
	u8 seen[MACHINE_MAX_STATEMENTS];

	for (u16 m = 0; m < sys->count; m++)
	{
		const machine_def_t* def = &sys->defs[m];

		for (u16 s = 0; s < def->state_count; s++)
		{
			const machine_state_def_t* state = &def->states[s];
			u16 ranges[3][2] = {
				{ state->on_enter_first, state->on_enter_count },
				{ state->on_tick_first,  state->on_tick_count },
				{ state->on_exit_first,  state->on_exit_count },
			};

			memset(seen, 0, sizeof(seen));
			for (u32 r = 0; r < 3; r++)
			{
				for (u32 i = 0; i < ranges[r][1]; i++)
					mark_point_owner(dev, def, (u16)(ranges[r][0] + i), seen);
			}
		}
		for (u16 w = 0; w < def->write_handler_count; w++)
		{
			const machine_write_handler_t* h = &def->write_handlers[w];

			memset(seen, 0, sizeof(seen));
			for (u32 i = 0; i < h->count; i++)
				mark_point_owner(dev, def, (u16)(h->first + i), seen);
		}
		if (def->has_state_register)
		{
			u32 point = device_find_point(dev, (space_kind_t)def->state_register_space,
						       def->state_register_address);

			if (point != DEVICE_MAX_POINTS)
				dev->points[point].kind = POINT_MACHINE;
		}
	}
}

static u64
deadline_at(const machine_transition_def_t* t, u64 entered_at, rng_t* rng)
{
	u64 offset = 0;

	if (t->after_kind == AFTER_LITERAL)
		offset = t->after_literal_ms;
	else if (t->after_kind == AFTER_RANDOM)
	{
		u64 span = t->after_max_ms - t->after_min_ms + 1;
		u64 limit = UINT64_MAX - (UINT64_MAX % span);

		do
			offset = rng_next(rng);
		while (offset >= limit);
		offset = t->after_min_ms + offset % span;
	}
	if (offset > UINT64_MAX - entered_at)
		return UINT64_MAX;
	return entered_at + offset;
}

static void
draw_deadlines(machine_system_t* sys, u16 machine_index, u16 state_index, u64 entered_at)
{
	const machine_def_t* def = &sys->defs[machine_index];

	for (u16 i = 0; i < def->transition_count; i++)
	{
		const machine_transition_def_t* t = &def->transitions[i];

		if (t->from != state_index || t->after_kind == AFTER_NONE)
		{
			sys->deadline[machine_index][i] = 0;
			continue;
		}
		sys->deadline[machine_index][i] = deadline_at(t, entered_at, &sys->rng);
	}
}

static void
warn_once(machine_system_t* sys, u16 machine_index, u16 state_index, u8 role,
	  expr_failure_t why)
{
	u8 bit = 0;
	u8* flags = NULL;

	if (why == EXPR_FAIL_NONE)
		why = EXPR_FAIL_TYPE;
	bit = (u8)(1u << (u8)why);
	flags = &sys->warned[machine_index][state_index][role];
	if (*flags & bit)
		return;
	*flags |= bit;
	LOG_WARN("machine '%s', state '%s', %s: %s",
		 sys->defs[machine_index].name,
		 sys->defs[machine_index].states[state_index].name,
		 handler_names[role], failure_names[why]);
}

static bool
exec_statements(device_t* dev, machine_system_t* sys, u16 machine_index, u16 state_index,
		u8 role, u16 first, u16 count, expr_ctx_t* ctx, u16 depth)
{
	const machine_def_t* def = &sys->defs[machine_index];

	if (depth >= MACHINE_MAX_STATEMENTS)
		return false;
	for (u32 i = 0; i < count; i++)
	{
		const machine_stmt_t* stmt = &def->statements[first + i];
		val_t value = { 0 };

		if (!expr_eval(def->nodes, stmt->expr_root, ctx, &value))
		{
			warn_once(sys, machine_index, state_index, role, ctx->failure);
			return false;
		}
		if (stmt->kind == STMT_SET)
		{
			if (stmt->target_kind == TARGET_VAR)
				sys->vars[machine_index][stmt->var_index] = value;
			else if (!device_set_point(dev, (space_kind_t)stmt->space, stmt->address, &value))
			{
				warn_once(sys, machine_index, state_index, role, EXPR_FAIL_POINT);
				return false;
			}
			continue;
		}

		if (stmt->kind == STMT_WHEN || stmt->kind == STMT_UNLESS)
		{
			bool take = value.type == VAL_BOOL && value.b;

			if (stmt->kind == STMT_UNLESS)
				take = !take;
			if (take && !exec_statements(dev, sys, machine_index, state_index, role,
						     stmt->first_child, stmt->child_count, ctx,
						     (u16)(depth + 1)))
				return false;
		}
	}
	return true;
}

static void
run_handler(device_t* dev, machine_system_t* sys, u16 machine_index, u16 state_index,
	    u8 role, u64 now_ms, u64 elapsed_ms)
{
	const machine_state_def_t* state = &sys->defs[machine_index].states[state_index];
	expr_ctx_t ctx = {
		.dev = dev,
		.vars = sys->vars[machine_index],
		.now_ms = now_ms,
		.elapsed_ms = elapsed_ms,
		.budget = MACHINE_EVAL_BUDGET,
	};
	u16 first = 0;
	u16 count = 0;

	if (role == MACHINE_ROLE_ENTER)
	{
		first = state->on_enter_first;
		count = state->on_enter_count;
	}
	else if (role == MACHINE_ROLE_TICK)
	{
		first = state->on_tick_first;
		count = state->on_tick_count;
	}
	else if (role == MACHINE_ROLE_EXIT)
	{
		first = state->on_exit_first;
		count = state->on_exit_count;
	}
	exec_statements(dev, sys, machine_index, state_index, role, first, count, &ctx, 0);
}

static u64
elapsed_since(u64 now_ms, u64 from_ms)
{
	return now_ms > from_ms ? now_ms - from_ms : 0;
}

static void
set_state_register(device_t* dev, machine_system_t* sys, u16 machine_index, u16 state_index)
{
	const machine_def_t* def = &sys->defs[machine_index];
	val_t value = { .type = VAL_NUM, .n = state_index };

	if (!def->has_state_register)
		return;
	device_set_point(dev, (space_kind_t)def->state_register_space,
			 def->state_register_address, &value);
}

static void
fire_transition(device_t* dev, machine_system_t* sys, u16 machine_index,
		u16 transition_index, u64 fire_ms)
{
	const machine_transition_def_t* t = &sys->defs[machine_index].transitions[transition_index];
	u16 old_state = sys->current[machine_index];

	run_handler(dev, sys, machine_index, old_state, MACHINE_ROLE_EXIT, fire_ms,
		    elapsed_since(fire_ms, sys->advanced_to[machine_index]));
	sys->current[machine_index] = t->to;
	set_state_register(dev, sys, machine_index, t->to);
	sys->entered_at[machine_index] = fire_ms;
	draw_deadlines(sys, machine_index, t->to, fire_ms);
	sys->advanced_to[machine_index] = fire_ms;
	run_handler(dev, sys, machine_index, t->to, MACHINE_ROLE_ENTER, fire_ms, 0);
}

static int
find_due_after(const machine_system_t* sys, u16 machine_index, u64 now_ms)
{
	const machine_def_t* def = &sys->defs[machine_index];
	int best = -1;

	for (u16 i = 0; i < def->transition_count; i++)
	{
		const machine_transition_def_t* t = &def->transitions[i];

		if (t->from != sys->current[machine_index] ||
		    t->has_when ||
		    t->after_kind == AFTER_NONE)
			continue;
		if (sys->deadline[machine_index][i] > now_ms)
			continue;
		if (best < 0 || sys->deadline[machine_index][i] < sys->deadline[machine_index][best])
			best = i;
	}
	return best;
}

static int
find_ready_when(device_t* dev, machine_system_t* sys, u16 machine_index, u64 now_ms)
{
	const machine_def_t* def = &sys->defs[machine_index];

	for (u16 i = 0; i < def->transition_count; i++)
	{
		const machine_transition_def_t* t = &def->transitions[i];
		expr_ctx_t ctx = {
			.dev = dev,
			.vars = sys->vars[machine_index],
			.now_ms = now_ms,
			.elapsed_ms = elapsed_since(now_ms, sys->advanced_to[machine_index]),
			.budget = MACHINE_EVAL_BUDGET,
		};
		val_t condition = { 0 };

		if (t->from != sys->current[machine_index] || !t->has_when)
			continue;
		if (t->after_kind != AFTER_NONE && sys->deadline[machine_index][i] > now_ms)
			continue;
		if (!expr_eval(def->nodes, t->when_root, &ctx, &condition))
		{
			warn_once(sys, machine_index, t->from, MACHINE_ROLE_WHEN, ctx.failure);
			continue; /* an aborted condition is false */
		}
		if (condition.b)
			return i;
	}
	return -1;
}

static void
cap_machine(machine_system_t* sys, u16 machine_index, u64 now_ms)
{
	sys->entered_at[machine_index] = now_ms;
	draw_deadlines(sys, machine_index, sys->current[machine_index], now_ms);
	sys->advanced_to[machine_index] = now_ms;
	if (!sys->cap_warned[machine_index])
	{
		sys->cap_warned[machine_index] = true;
		LOG_WARN("machine '%s': reached %d firings in one tick",
			 sys->defs[machine_index].name, MACHINE_MAX_FIRINGS);
	}
}

void
device_machines_advance(device_t* dev, u64 now_ms)
{
	machine_system_t* sys = NULL;

	if (!dev || !dev->machines || !dev->machines->finalized)
		return;
	sys = dev->machines;

	for (u16 m = 0; m < sys->count; m++)
	{
		u32 firings = 0;

		for (;;)
		{
			int t = find_due_after(sys, m, now_ms);

			if (t < 0)
				t = find_ready_when(dev, sys, m, now_ms);
			if (t >= 0)
			{
				u64 fire_ms = sys->deadline[m][t];

				if (sys->defs[m].transitions[t].has_when ||
				    sys->defs[m].transitions[t].after_kind == AFTER_NONE)
					fire_ms = now_ms;
				fire_transition(dev, sys, m, (u16)t, fire_ms);
				if (++firings >= MACHINE_MAX_FIRINGS)
				{
					cap_machine(sys, m, now_ms);
					break;
				}
				continue;
			}
			run_handler(dev, sys, m, sys->current[m], MACHINE_ROLE_TICK, now_ms,
				    elapsed_since(now_ms, sys->advanced_to[m]));
			sys->advanced_to[m] = now_ms;
			break;
		}
	}
}

void
device_machines_on_write(device_t* dev, space_kind_t space, u32 address,
			 u32 qty, u64 now_ms)
{
	machine_system_t* sys = NULL;

	if (!dev || !dev->machines || !dev->machines->finalized || qty == 0)
		return;
	sys = dev->machines;
	for (u16 m = 0; m < sys->count; m++)
	{
		const machine_def_t* def = &sys->defs[m];
		u16 state = sys->current[m];

		if (state >= def->state_count)
			continue;
		for (u16 w = 0; w < def->write_handler_count; w++)
		{
			const machine_write_handler_t* h = &def->write_handlers[w];
			expr_ctx_t ctx = {
				.dev = dev,
				.vars = sys->vars[m],
				.now_ms = now_ms,
				.elapsed_ms = elapsed_since(now_ms, sys->advanced_to[m]),
				.budget = MACHINE_EVAL_BUDGET,
			};
			u32 index = DEVICE_MAX_POINTS;
			u32 width = 0;
			u64 watched_end = 0;

			if ((space_kind_t)h->space != space)
				continue;
			index = device_find_point(dev, space, h->address);
			if (index == DEVICE_MAX_POINTS)
				continue;
			width = value_width(dev->points[index].type);
			watched_end = (u64)h->address + width;
			if ((u64)h->address >= (u64)address + qty ||
			    (u64)address >= watched_end)
				continue;
			exec_statements(dev, sys, m, state, MACHINE_ROLE_WRITE,
					h->first, h->count, &ctx, 0);
		}
	}
}

fault_set_t
device_effective_faults(const device_t* dev)
{
	fault_set_t effective = { 0 };
	const machine_system_t* sys = NULL;
	u8 overridden = 0;

	if (!dev)
		return effective;
	effective = dev->faults;
	sys = dev->machines;
	if (!sys || !sys->finalized)
		return effective;
	for (u16 m = 0; m < sys->count; m++)
	{
		const machine_def_t* def = &sys->defs[m];
		u16 state = sys->current[m];
		const fault_set_t* overlay = NULL;

		if (state >= def->state_count)
			continue;
		overlay = &def->states[state].faults;
		for (u8 kind = 0; kind < FAULT_KIND_COUNT; kind++)
		{
			u8 bit = FAULT_BIT(kind);

			if ((overlay->present & bit) == 0 || (overridden & bit) != 0)
				continue;
			effective.by_kind[kind] = overlay->by_kind[kind];
			effective.present |= bit;
			overridden |= bit;
		}
	}
	return effective;
}

bool
device_machine_fault_present(const device_t* dev, fault_kind_t kind)
{
	const machine_system_t* sys = NULL;
	u8 bit = 0;

	if (!dev || !dev->machines || kind >= FAULT_KIND_COUNT)
		return false;
	bit = FAULT_BIT(kind);
	sys = dev->machines;
	for (u16 m = 0; m < sys->count; m++)
	{
		const machine_def_t* def = &sys->defs[m];

		for (u16 s = 0; s < def->state_count; s++)
		{
			if ((def->states[s].faults.present & bit) != 0)
				return true;
		}
	}
	return false;
}

const char*
device_machines_prepare(device_t* dev)
{
	if (!dev)
		return "device has no machine storage";
	if (dev->machines)
		return NULL;
	if (!dev->arena)
		return "device has no arena for machine storage";
	dev->machines = ARENA_PUSH_STRUCT(dev->arena, machine_system_t);
	if (!dev->machines)
		return "out of memory while creating machine storage";
	return NULL;
}

const char*
device_add_machine(device_t* dev, const machine_def_t* def)
{
	machine_system_t* sys = NULL;
	const char* error = NULL;

	error = device_machines_prepare(dev);
	if (error)
		return error;
	sys = dev->machines;
	if (sys->finalized)
		return "cannot add machines after finalization";
	if (!def)
		return "machine definition is missing";
	if (sys->count >= MACHINE_MAX_MACHINES)
		return "too many machines on one device (max 8)";
	sys->defs[sys->count++] = *def;
	return NULL;
}

void
device_seed_machines(device_t* dev, u64 server_seed)
{
	machine_system_t* sys = NULL;

	if (!dev || !dev->machines)
		return;
	sys = dev->machines;
	sys->seed = server_seed;
	rng_seed(&sys->rng, server_seed ^ ((u64)dev->unit_id * MACHINE_SEED_FACTOR));
}

const char*
device_finalize_machines(device_t* dev)
{
	validation_t validation = { 0 };
	machine_system_t* sys = NULL;
	const char* error = NULL;

	if (!dev)
		return "device has no machine storage";
	if (!dev->machines)
		return NULL;
	sys = dev->machines;
	validation.dev  = dev;
	validation.sys = sys;
	error = collect_state_registers(&validation);
	if (error)
		return error;

	for (u16 m = 0; m < sys->count; m++)
	{
		error = validate_machine(&validation, m);
		if (error)
			return error;
	}
	error = validate_write_handlers(&validation);
	if (error)
		return error;
	mark_point_owners(dev, sys);
	rng_seed(&sys->rng, sys->seed ^ ((u64)dev->unit_id * MACHINE_SEED_FACTOR));

	for (u16 m = 0; m < sys->count; m++)
	{
		const machine_def_t* def = &sys->defs[m];

		sys->current[m]    = def->initial_state;
		sys->entered_at[m] = dev->start_ms;
		sys->advanced_to[m] = dev->start_ms;
		for (u16 v = 0; v < def->var_count; v++)
			sys->vars[m][v] = def->vars[v].initial;
		memset(sys->warned[m], 0, sizeof(sys->warned[m]));
		sys->cap_warned[m] = false;
		draw_deadlines(sys, m, def->initial_state, dev->start_ms);
		set_state_register(dev, sys, m, def->initial_state);
		run_handler(dev, sys, m, def->initial_state, MACHINE_ROLE_ENTER, dev->start_ms, 0);
		sys->advanced_to[m] = dev->start_ms;
	}
	sys->finalized = true;
	return NULL;
}
