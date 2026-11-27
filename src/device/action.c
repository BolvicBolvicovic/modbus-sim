/**
 * @file
 * @brief Point actions: storage, execution and parameters.
 */
#include "device/action.h"
#include "util/log.h"
#include <stdio.h>
#include <string.h>

static char error_text[256];

static bool
before(space_kind_t space, u32 address, action_event_t event, const action_instance_t* other)
{
	if (space != other->space)
		return space < other->space;
	if (address != other->address)
		return address < other->address;
	return event < other->event;
}

const char*
device_add_action(device_t* dev, space_kind_t space, u32 address, action_event_t event,
		  const char* name, u16 param_count, const machine_def_t* body)
{
	action_system_t*	sys = NULL;
	action_instance_t*	inst = NULL;
	u16			pos = 0;

	if (!dev || !dev->arena || !body)
		return "device has no storage for point actions";
	if (!dev->actions)
	{
		dev->actions = ARENA_PUSH_ARRAY_ALIGNED(dev->arena, action_system_t, 1,
							_Alignof(action_system_t));
		if (!dev->actions)
			return "out of memory while creating action storage";
	}
	sys = dev->actions;
	if (sys->count >= ACTION_MAX_INSTANCES)
		return "too many point actions on one device (max 256)";
	while (pos < sys->count &&
	       !before(space, address, event, &sys->items[pos]))
	{
		if (sys->items[pos].space == space &&
		    sys->items[pos].address == address &&
		    sys->items[pos].event == event)
			return "the point already has this action";
		pos++;
	}
	memmove(&sys->items[pos + 1], &sys->items[pos],
		(size_t)(sys->count - pos) * sizeof(sys->items[0]));
	sys->count++;

	inst = &sys->items[pos];
	memset(inst, 0, sizeof(*inst));
	inst->space = space;
	inst->address = address;
	inst->event = event;
	snprintf(inst->name, sizeof(inst->name), "%s", name ? name : "");
	inst->param_count = param_count;
	inst->var_count = body->var_count;
	inst->top_count = body->states[0].on_tick_count;
	inst->statement_count = body->statement_count;
	inst->node_count = body->node_count;
	inst->statements = ARENA_PUSH_ARRAY(dev->arena, machine_stmt_t, body->statement_count + 1u);
	inst->nodes = ARENA_PUSH_ARRAY(dev->arena, expr_node_t, body->node_count + 1u);
	inst->defs = ARENA_PUSH_ARRAY(dev->arena, machine_var_def_t, body->var_count + 1u);
	inst->vars = ARENA_PUSH_ARRAY(dev->arena, val_t, body->var_count + 1u);
	if (!inst->statements || !inst->nodes || !inst->defs || !inst->vars)
	{
		memmove(&sys->items[pos], &sys->items[pos + 1],
			(size_t)(sys->count - pos - 1) * sizeof(sys->items[0]));
		sys->count--;
		return "out of memory while storing a point action";
	}
	memcpy(inst->statements, body->statements, body->statement_count * sizeof(machine_stmt_t));
	memcpy(inst->nodes, body->nodes, body->node_count * sizeof(expr_node_t));
	memcpy(inst->defs, body->vars, body->var_count * sizeof(machine_var_def_t));
	for (u16 i = 0; i < inst->var_count; i++)
		inst->vars[i] = inst->defs[i].initial;
	return NULL;
}

const char*
device_finalize_actions(const device_t* dev)
{
	if (!dev || !dev->actions)
		return NULL;
	for (u16 a = 0; a < dev->actions->count; a++)
	{
		const action_instance_t* inst = &dev->actions->items[a];

		for (u16 s = 0; s < inst->statement_count; s++)
		{
			const machine_stmt_t* stmt = &inst->statements[s];
			u32 index = 0;

			if (stmt->kind != STMT_SET || stmt->target_kind != TARGET_POINT)
				continue;
			index = device_find_point(dev, (space_kind_t)stmt->space, stmt->address);
			if (index != DEVICE_MAX_POINTS &&
			    dev->points[index].kind != POINT_MACHINE)
				continue;
			snprintf(error_text, sizeof(error_text),
				 "%s %u %s: set! target %s %u is driven by a machine",
				 space_name(inst->space), inst->address,
				 inst->event == ACTION_ON_READ ? "on-read" : "on-write",
				 space_name((space_kind_t)stmt->space), stmt->address);
			return error_text;
		}
	}
	return NULL;
}

static bool
run_instance(device_t* dev, action_instance_t* inst)
{
	expr_ctx_t ctx = {
		.dev = dev,
		.vars = inst->vars,
		.now_ms = dev->now_ms,
		.elapsed_ms = inst->ran && dev->now_ms > inst->last_ms ? dev->now_ms - inst->last_ms : 0,
		.budget = MACHINE_EVAL_BUDGET,
	};
	bool ok = machine_exec_statements(dev, inst->statements, inst->nodes, 0, inst->top_count,
					  &ctx, true, 0);

	inst->last_ms = dev->now_ms;
	inst->ran = true;
	if (!ok)
	{
		expr_failure_t why = ctx.failure == EXPR_FAIL_NONE ? EXPR_FAIL_TYPE : ctx.failure;
		u8 bit = (u8)(1u << (u8)why);

		if ((inst->warned & bit) == 0)
		{
			inst->warned |= bit;
			LOG_WARN("device unit %u, %s %u %s%s%s%s: %s", dev->unit_id,
				 space_name(inst->space), inst->address,
				 inst->event == ACTION_ON_READ ? "on-read" : "on-write",
				 inst->name[0] ? " (action '" : "", inst->name,
				 inst->name[0] ? "')" : "", expr_failure_text(why));
		}
	}
	return ok;
}

static bool
run_overlapping(device_t* dev, action_event_t event, space_kind_t space, u32 address, u32 count)
{
	action_system_t* sys = dev ? dev->actions : NULL;
	bool ok = true;

	if (!sys || count == 0)
		return true;
	for (u16 i = 0; i < sys->count; i++)
	{
		action_instance_t* inst = &sys->items[i];
		u32 index = 0;
		u64 end = 0;

		if (inst->space != space ||
		    inst->event != event ||
		    (u64)inst->address >= (u64)address + count)
			continue;
		index = device_find_point(dev, space, inst->address);
		if (index == DEVICE_MAX_POINTS)
			continue;
		end = (u64)inst->address + point_width(&dev->points[index]);
		if ((u64)address >= end)
			continue;
		/* Every overlapping action runs even after a failure; the request still fails. */
		if (!run_instance(dev, inst))
			ok = false;
	}
	return ok;
}

bool
device_actions_on_read(device_t* dev, space_kind_t space, u32 address, u32 count)
{
	return run_overlapping(dev, ACTION_ON_READ, space, address, count);
}

bool
device_actions_on_write(device_t* dev, space_kind_t space, u32 address, u32 count)
{
	return run_overlapping(dev, ACTION_ON_WRITE, space, address, count);
}

void
device_actions_restart(device_t* dev)
{
	if (!dev || !dev->actions)
		return;
	for (u16 i = 0; i < dev->actions->count; i++)
	{
		action_instance_t* inst = &dev->actions->items[i];

		for (u16 v = inst->param_count; v < inst->var_count; v++)
			inst->vars[v] = inst->defs[v].initial;
		inst->ran = false;
		inst->last_ms = 0;
		inst->warned = 0;
	}
}

const action_instance_t*
device_action_at(const device_t* dev, u32 index)
{
	if (!dev || !dev->actions || index >= dev->actions->count)
		return NULL;
	return &dev->actions->items[index];
}

static val_t*
find_param(const device_t* dev, space_kind_t space, u32 address, action_event_t event,
	   const char* name)
{
	if (!dev || !dev->actions || !name)
		return NULL;
	for (u16 i = 0; i < dev->actions->count; i++)
	{
		action_instance_t* inst = &dev->actions->items[i];

		if (inst->space != space ||
		    inst->address != address ||
		    inst->event != event)
			continue;
		for (u16 p = 0; p < inst->param_count; p++)
		{
			if (strcmp(inst->defs[p].name, name) == 0)
				return &inst->vars[p];
		}
		return NULL;
	}
	return NULL;
}

bool
device_action_param_get(const device_t* dev, space_kind_t space, u32 address,
			action_event_t event, const char* name, val_t* out)
{
	const val_t* value = find_param(dev, space, address, event, name);

	if (!value || !out)
		return false;
	*out = *value;
	return true;
}

bool
device_action_param_set(device_t* dev, space_kind_t space, u32 address,
			action_event_t event, const char* name, const val_t* in)
{
	val_t* value = find_param(dev, space, address, event, name);

	if (!value || !in || in->type != value->type)
		return false;
	*value = *in;
	return true;
}
