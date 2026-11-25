#include "vtest.h"
#include "device/machine.h"
#include "util/rng.h"
#include "vlib/macros.h"
#include <string.h>

#define AFTER_MAX_MS 86400000ULL

static machine_def_t*
new_def(arena_t* arena, const char* name)
{
	machine_def_t* def = ARENA_PUSH_STRUCT(arena, machine_def_t);

	ASSERT_SUCCESS(def != NULL);
	memset(def, 0, sizeof(*def));
	snprintf(def->name, sizeof(def->name), "%s", name);
	return def;
}

static machine_def_t*
simple_def(arena_t* arena)
{
	machine_def_t* def = new_def(arena, "m");

	def->states[0].name[0] = 's';
	def->state_count = 1;
	return def;
}

static void
add_state(machine_def_t* def, const char* name)
{
	u16 index = def->state_count++;

	snprintf(def->states[index].name, sizeof(def->states[index].name), "%s", name);
}

static u16
add_var(machine_def_t* def, const char* name, val_t initial)
{
	u16 index = def->var_count++;

	(void)name;
	snprintf(def->vars[index].name, sizeof(def->vars[index].name), "var%u", (u32)index);
	def->vars[index].initial = initial;
	return index;
}

static u16
add_num_var(machine_def_t* def, const char* name, f64 value)
{
	return add_var(def, name, (val_t){ .type = VAL_NUM, .n = value });
}

static u16
add_bool_var(machine_def_t* def, const char* name, bool value)
{
	return add_var(def, name, (val_t){ .type = VAL_BOOL, .b = value });
}

static expr_node_t
plain_node(expr_kind_t kind)
{
	return (expr_node_t){
		.kind = kind,
		.a = EXPR_NO_CHILD,
		.b = EXPR_NO_CHILD,
		.c = EXPR_NO_CHILD,
	};
}

static u16
node_num(machine_def_t* def, f64 value)
{
	u16 index = def->node_count++;

	def->nodes[index] = plain_node(EXPR_NUM);
	def->nodes[index].literal = (val_t){ .type = VAL_NUM, .n = value };
	return index;
}

static u16
node_bool(machine_def_t* def, bool value)
{
	u16 index = def->node_count++;

	def->nodes[index] = plain_node(EXPR_BOOL);
	def->nodes[index].literal = (val_t){ .type = VAL_BOOL, .b = value };
	return index;
}

static u16
node_var(machine_def_t* def, u16 var_index)
{
	u16 index = def->node_count++;

	def->nodes[index] = plain_node(EXPR_VAR);
	def->nodes[index].a = var_index;
	return index;
}

static u16
node_point(machine_def_t* def, space_kind_t space, u32 address)
{
	u16 index = def->node_count++;

	def->nodes[index] = plain_node(EXPR_POINT);
	def->nodes[index].space = (u8)space;
	def->nodes[index].address = address;
	return index;
}

static u16
node_binary(machine_def_t* def, expr_kind_t kind, u16 left, u16 right)
{
	u16 index = def->node_count++;

	def->nodes[index] = plain_node(kind);
	def->nodes[index].a = left;
	def->nodes[index].b = right;
	return index;
}

static u16
node_if(machine_def_t* def, u16 condition, u16 then_index, u16 else_index)
{
	u16 index = def->node_count++;

	def->nodes[index] = plain_node(EXPR_IF);
	def->nodes[index].a = condition;
	def->nodes[index].b = then_index;
	def->nodes[index].c = else_index;
	return index;
}

static u16
node_time(machine_def_t* def, bool elapsed)
{
	u16 index = def->node_count++;

	def->nodes[index] = plain_node(elapsed ? EXPR_ELAPSED_MS : EXPR_NOW_MS);
	return index;
}

static u16
set_var_stmt(machine_def_t* def, u16 var_index, u16 expression)
{
	u16 index = def->statement_count++;

	memset(&def->statements[index], 0, sizeof(def->statements[index]));
	def->statements[index].kind = STMT_SET;
	def->statements[index].expr_root = expression;
	def->statements[index].target_kind = TARGET_VAR;
	def->statements[index].var_index = var_index;
	def->statements[index].first_child = EXPR_NO_CHILD;
	return index;
}

static u16
set_point_stmt(machine_def_t* def, space_kind_t space, u32 address, u16 expression)
{
	u16 index = def->statement_count++;

	memset(&def->statements[index], 0, sizeof(def->statements[index]));
	def->statements[index].kind = STMT_SET;
	def->statements[index].expr_root = expression;
	def->statements[index].target_kind = TARGET_POINT;
	def->statements[index].space = (u8)space;
	def->statements[index].address = address;
	def->statements[index].first_child = EXPR_NO_CHILD;
	return index;
}

static u16
guard_stmt(machine_def_t* def, machine_stmt_kind_t kind, u16 condition,
	   u16 first_child, u16 child_count)
{
	u16 index = def->statement_count++;

	memset(&def->statements[index], 0, sizeof(def->statements[index]));
	def->statements[index].kind = kind;
	def->statements[index].expr_root = condition;
	def->statements[index].first_child = first_child;
	def->statements[index].child_count = child_count;
	return index;
}

static machine_transition_def_t*
add_transition(machine_def_t* def, u16 from, u16 to)
{
	machine_transition_def_t* t = &def->transitions[def->transition_count++];

	memset(t, 0, sizeof(*t));
	t->from = from;
	t->to = to;
	return t;
}

static void
add_after(machine_def_t* def, u16 from, u16 to, u64 ms)
{
	machine_transition_def_t* t = add_transition(def, from, to);

	t->after_kind = AFTER_LITERAL;
	t->after_literal_ms = ms;
}

static void
add_random(machine_def_t* def, u16 from, u16 to, u64 low, u64 high)
{
	machine_transition_def_t* t = add_transition(def, from, to);

	t->after_kind = AFTER_RANDOM;
	t->after_min_ms = low;
	t->after_max_ms = high;
}

static void
add_when(machine_def_t* def, u16 from, u16 to, u16 condition)
{
	machine_transition_def_t* t = add_transition(def, from, to);

	t->has_when = true;
	t->when_root = condition;
}

static point_t
static_point(space_kind_t space, u32 address, value_type_t type, f64 value)
{
	point_t point = { 0 };

	point.space = space;
	point.address = address;
	point.type = type;
	point.kind = POINT_STATIC;
	point.static_value = value;
	return point;
}

static void
prepare(device_t* dev, u64 seed)
{
	device_seed_machines(dev, seed);
	ASSERT_SUCCESS(device_finalize_machines(dev) == NULL);
}

static void
expect_def_error(arena_t* arena, const machine_def_t* def)
{
	device_t* dev = device_new(arena, 0);

	ASSERT_SUCCESS(dev != NULL);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	device_seed_machines(dev, 0);
	ASSERT_FAILURE(device_finalize_machines(dev) == NULL);
}

static u64
num_var_value(const device_t* dev, u16 machine_index, u16 var_index)
{
	return (u64)dev->machines->vars[machine_index][var_index].n;
}

TEST(MachineBigTickEqualsManySmallTicks)
{
	arena_t* arena  = ARENA_ALLOC();
	device_t* once  = device_new(arena, 1000);
	device_t* steps = device_new(arena, 1000);
	machine_def_t* def = new_def(arena, "clock");

	add_state(def, "idle");
	add_state(def, "done");
	add_after(def, 0, 1, 10000);
	ASSERT_SUCCESS(device_add_machine(once, def) == NULL);
	ASSERT_SUCCESS(device_add_machine(steps, def) == NULL);
	prepare(once, 5);
	prepare(steps, 5);

	device_tick(once, 11000);
	for (u32 i = 1; i <= 1000; i++)
		device_tick(steps, 1000 + i * 10);

	ASSERT_EQ(once->machines->current[0] == 1 && once->machines->entered_at[0] == 11000);
	ASSERT_EQ(once->machines->current[0] == steps->machines->current[0]);
	ASSERT_EQ(once->machines->entered_at[0] == steps->machines->entered_at[0]);
	ASSERT_EQ(once->machines->advanced_to[0] == steps->machines->advanced_to[0]);
	arena_release(arena);
}

TEST(MachineWhenTransitionsUseDeclarationOrder)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* first = device_new(arena, 0);
	device_t* second = device_new(arena, 0);
	machine_def_t* a = new_def(arena, "first");
	machine_def_t* b = new_def(arena, "second");

	add_state(a, "s0");
	add_state(a, "s1");
	add_state(a, "s2");
	add_when(a, 0, 1, node_bool(a, false));
	add_when(a, 0, 2, node_bool(a, true));
	add_state(b, "s0");
	add_state(b, "s1");
	add_state(b, "s2");
	add_when(b, 0, 1, node_bool(b, true));
	add_when(b, 0, 2, node_bool(b, false));

	ASSERT_SUCCESS(device_add_machine(first, a) == NULL);
	ASSERT_SUCCESS(device_add_machine(second, b) == NULL);
	prepare(first, 0);
	prepare(second, 0);
	device_tick(first, 10);
	device_tick(second, 10);
	ASSERT_EQ(first->machines->current[0] == 2);
	ASSERT_EQ(second->machines->current[0] == 1);
	ASSERT_EQ(first->machines->entered_at[0] == 10);
	arena_release(arena);
}

TEST(MachineEarliestAfterDeadlineFiresAndTiesKeepOrder)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 500);
	machine_def_t* def = new_def(arena, "deadlines");

	add_state(def, "start");
	add_state(def, "second");
	add_state(def, "first-tie");
	add_state(def, "last-tie");
	add_after(def, 0, 1, 200);
	add_after(def, 0, 2, 100);
	add_after(def, 0, 3, 100);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 900);
	ASSERT_EQ(dev->machines->current[0] == 2);
	ASSERT_EQ(dev->machines->entered_at[0] == 600);
	ASSERT_EQ(dev->machines->advanced_to[0] == 900);
	arena_release(arena);
}

TEST(MachineWhenAfterTransitionRequiresBothConditions)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "gated");
	machine_transition_def_t* t = NULL;

	add_state(def, "idle");
	add_state(def, "run");
	t = add_transition(def, 0, 1);
	t->has_when = true;
	t->when_root = node_bool(def, true);
	t->after_kind = AFTER_LITERAL;
	t->after_literal_ms = 1000;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 500);
	ASSERT_EQ(dev->machines->current[0] == 0);
	device_tick(dev, 1000);
	ASSERT_EQ(dev->machines->current[0] == 1 && dev->machines->entered_at[0] == 1000);
	arena_release(arena);
}

TEST(MachineDueAfterTransitionWinsOverReadyWhenTransition)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 1000);
	machine_def_t* def = new_def(arena, "priority");

	add_state(def, "start");
	add_state(def, "after");
	add_state(def, "when");
	add_after(def, 0, 1, 1000);
	add_when(def, 0, 2, node_bool(def, true));
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	/* Both transitions are due; the after-only one fires first, at its deadline. */
	device_tick(dev, 2500);
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_EQ(dev->machines->entered_at[0] == 2000);
	arena_release(arena);
}

TEST(MachineHandlersObserveOrderNowAndElapsed)
{
	enum {
		V_ENTER_NOW, V_ENTER_ELAPSED, V_TICK_NOW, V_TICK_ELAPSED,
		V_EXIT_NOW, V_EXIT_ELAPSED, V_ORDER, V_TICK_COUNT, V_COUNT,
	};
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 5000);
	machine_def_t* def = new_def(arena, "observer");
	u16 one = 0;
	u16 three_node = 0;
	u16 ten = 0;
	u16 now = 0;
	u16 elapsed = 0;
	u16 order_node = 0;
	u16 next_order = 0;
	u16 tick_count_node = 0;
	u16 next_tick_count = 0;
	u16 first = 0;

	add_state(def, "run");
	add_state(def, "done");
	for (u16 i = 0; i < V_COUNT; i++)
		add_num_var(def, "v", 0);
	one = node_num(def, 1);
	three_node = node_num(def, 3);
	ten = node_num(def, 10);
	now = node_time(def, false);
	elapsed = node_time(def, true);
	order_node = node_var(def, V_ORDER);
	next_order = node_binary(def, EXPR_ADD, order_node, ten);
	tick_count_node = node_var(def, V_TICK_COUNT);
	next_tick_count = node_binary(def, EXPR_ADD, tick_count_node, one);

	first = def->statement_count;
	set_var_stmt(def, V_ORDER, one);
	set_var_stmt(def, V_ENTER_NOW, now);
	set_var_stmt(def, V_ENTER_ELAPSED, elapsed);
	def->states[0].on_enter_first = first;
	def->states[0].on_enter_count = 3;

	first = def->statement_count;
	set_var_stmt(def, V_TICK_NOW, now);
	set_var_stmt(def, V_TICK_ELAPSED, elapsed);
	set_var_stmt(def, V_ORDER, one);
	set_var_stmt(def, V_TICK_COUNT, next_tick_count);
	def->states[0].on_tick_first = first;
	def->states[0].on_tick_count = 4;

	first = def->statement_count;
	set_var_stmt(def, V_EXIT_NOW, now);
	set_var_stmt(def, V_EXIT_ELAPSED, elapsed);
	set_var_stmt(def, V_ORDER, three_node);
	def->states[0].on_exit_first = first;
	def->states[0].on_exit_count = 3;

	first = def->statement_count;
	set_var_stmt(def, V_ORDER, next_order);
	set_var_stmt(def, V_ENTER_NOW, now);
	set_var_stmt(def, V_ENTER_ELAPSED, elapsed);
	def->states[1].on_enter_first = first;
	def->states[1].on_enter_count = 3;

	first = def->statement_count;
	set_var_stmt(def, V_TICK_NOW, now);
	set_var_stmt(def, V_TICK_ELAPSED, elapsed);
	set_var_stmt(def, V_TICK_COUNT, next_tick_count);
	def->states[1].on_tick_first = first;
	def->states[1].on_tick_count = 3;

	add_after(def, 0, 1, 1000);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	ASSERT_EQ(num_var_value(dev, 0, V_ORDER) == 1);
	ASSERT_EQ(num_var_value(dev, 0, V_ENTER_NOW) == 5000);
	ASSERT_EQ(num_var_value(dev, 0, V_ENTER_ELAPSED) == 0);

	device_tick(dev, 6000);
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_EQ(num_var_value(dev, 0, V_EXIT_NOW) == 6000);
	ASSERT_EQ(num_var_value(dev, 0, V_EXIT_ELAPSED) == 1000);
	ASSERT_EQ(num_var_value(dev, 0, V_ORDER) == 13); /* exit set 3, enter added 10 */
	ASSERT_EQ(num_var_value(dev, 0, V_ENTER_NOW) == 6000);
	ASSERT_EQ(num_var_value(dev, 0, V_ENTER_ELAPSED) == 0);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_NOW) == 6000);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_ELAPSED) == 0);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_COUNT) == 1);

	device_tick(dev, 8000);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_NOW) == 8000);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_ELAPSED) == 2000);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_COUNT) == 2);
	arena_release(arena);
}

TEST(MachineOnExitElapsedUsesLastTickNotStateEntry)
{
	enum { V_TICK_ELAPSED, V_TICK_NOW, V_EXIT_ELAPSED, V_EXIT_NOW, V_COUNT };
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 1000);
	machine_def_t* def = new_def(arena, "elapsed");
	u16 elapsed = 0;
	u16 now = 0;
	u16 first = 0;

	add_state(def, "waiting");
	add_state(def, "done");
	for (u16 i = 0; i < V_COUNT; i++)
		add_num_var(def, "v", 0);
	elapsed = node_time(def, true);
	now = node_time(def, false);

	first = def->statement_count;
	set_var_stmt(def, V_TICK_ELAPSED, elapsed);
	set_var_stmt(def, V_TICK_NOW, now);
	def->states[0].on_tick_first = first;
	def->states[0].on_tick_count = 2;

	first = def->statement_count;
	set_var_stmt(def, V_EXIT_ELAPSED, elapsed);
	set_var_stmt(def, V_EXIT_NOW, now);
	def->states[0].on_exit_first = first;
	def->states[0].on_exit_count = 2;

	add_after(def, 0, 1, 1000);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 1500); /* last tick: elapsed 500, distinct from state entry at 1000 */
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_ELAPSED) == 500);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_NOW) == 1500);
	ASSERT_EQ(dev->machines->entered_at[0] == 1000);
	ASSERT_EQ(dev->machines->advanced_to[0] == 1500);

	device_tick(dev, 2500); /* after transition fires at deadline 2000 */
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_EQ(dev->machines->entered_at[0] == 2000);
	ASSERT_EQ(dev->machines->advanced_to[0] == 2500);
	ASSERT_EQ(num_var_value(dev, 0, V_EXIT_ELAPSED) == 500);
	ASSERT_EQ(num_var_value(dev, 0, V_EXIT_NOW) == 2000);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_ELAPSED) == 500);
	ASSERT_EQ(num_var_value(dev, 0, V_TICK_NOW) == 1500); /* new state has no on-tick */
	arena_release(arena);
}

TEST(MachineSeesMasterWritesOnNextTick)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "copy");
	point_t input = static_point(SPACE_HOLDING, 10, VT_U16, 7);
	point_t output = static_point(SPACE_HOLDING, 11, VT_U16, 0);
	u16 words = 0;

	ASSERT_SUCCESS(device_add_point(dev, &input) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &output) == NULL);
	add_state(def, "copying");
	set_point_stmt(def, SPACE_HOLDING, 11,
		       node_binary(def, EXPR_ADD, node_point(def, SPACE_HOLDING, 10),
				   node_num(def, 1)));
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);
	ASSERT_EQ(dev->points[0].kind == POINT_STATIC);
	ASSERT_EQ(dev->points[1].kind == POINT_MACHINE);

	words = 20;
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 10, 1, &words));
	device_tick(dev, 100);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 11, 1, &words));
	ASSERT_EQ(words == 21);

	words = 30;
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 10, 1, &words));
	device_tick(dev, 200);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 11, 1, &words));
	ASSERT_EQ(words == 31);
	arena_release(arena);
}

TEST(MachineSeesRampAfterTick)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 1000);
	machine_def_t* def = new_def(arena, "ramp-reader");
	point_t ramp = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	point_t output = static_point(SPACE_HOLDING, 1, VT_U16, 0);
	u16 words = 0;

	ramp.kind = POINT_RAMP;
	ramp.ramp = (ramp_params_t){ .from = 0, .to = 100, .step = 10,
				     .every_ms = 1000, .mode = RAMP_HOLD };
	ASSERT_SUCCESS(device_add_point(dev, &ramp) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &output) == NULL);
	add_state(def, "s");
	set_point_stmt(def, SPACE_HOLDING, 1, node_point(def, SPACE_HOLDING, 0));
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 4500);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &words));
	ASSERT_EQ(words == 30); /* ramp ran first, at 3000 ms after start */
	arena_release(arena);
}

TEST(MachinesAdvanceInDeclarationOrder)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* forward = device_new(arena, 0);
	device_t* reverse = device_new(arena, 0);
	machine_def_t* producer = new_def(arena, "producer");
	machine_def_t* consumer = new_def(arena, "consumer");
	point_t first = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	point_t second = static_point(SPACE_HOLDING, 1, VT_U16, 0);
	u16 words = 0;

	add_state(producer, "s");
	set_point_stmt(producer, SPACE_HOLDING, 0, node_num(producer, 5));
	producer->states[0].on_tick_first = 0;
	producer->states[0].on_tick_count = 1;
	add_state(consumer, "s");
	set_point_stmt(consumer, SPACE_HOLDING, 1,
		       node_binary(consumer, EXPR_ADD, node_point(consumer, SPACE_HOLDING, 0),
				   node_num(consumer, 1)));
	consumer->states[0].on_tick_first = 0;
	consumer->states[0].on_tick_count = 1;

	for (u32 i = 0; i < 2; i++)
	{
		device_t* dev = i == 0 ? forward : reverse;

		ASSERT_SUCCESS(device_add_point(dev, &first) == NULL);
		ASSERT_SUCCESS(device_add_point(dev, &second) == NULL);
	}
	ASSERT_SUCCESS(device_add_machine(forward, producer) == NULL);
	ASSERT_SUCCESS(device_add_machine(forward, consumer) == NULL);
	ASSERT_SUCCESS(device_add_machine(reverse, consumer) == NULL);
	ASSERT_SUCCESS(device_add_machine(reverse, producer) == NULL);
	prepare(forward, 0);
	prepare(reverse, 0);

	device_tick(forward, 10);
	device_tick(reverse, 10);
	ASSERT_SUCCESS(device_read(forward, SPACE_HOLDING, 1, 1, &words));
	ASSERT_EQ(words == 6);
	ASSERT_SUCCESS(device_read(reverse, SPACE_HOLDING, 1, 1, &words));
	ASSERT_EQ(words == 1); /* consumer ran before producer */
	arena_release(arena);
}

TEST(MachineRandomDeadlinesAreDeterministicAndPrivate)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* devices[4];
	machine_def_t* def = new_def(arena, "random");
	rng_t fault_rng;
	u64 deadline = 0;

	add_state(def, "waiting");
	add_state(def, "done");
	add_random(def, 0, 1, 100, 1000);
	for (u32 i = 0; i < 4; i++)
	{
		devices[i] = device_new(arena, 5000);
		ASSERT_SUCCESS(devices[i] != NULL);
		ASSERT_SUCCESS(device_add_machine(devices[i], def) == NULL);
	}
	devices[3]->unit_id = 2;
	device_seed_machines(devices[0], 123);
	device_seed_machines(devices[1], 123);
	device_seed_machines(devices[2], 124);
	device_seed_machines(devices[3], 123);
	rng_seed(&fault_rng, 999);
	for (u32 i = 0; i < 20; i++)
		rng_next(&fault_rng);
	device_seed_machines(devices[0], 123);
	ASSERT_SUCCESS(device_finalize_machines(devices[0]) == NULL);
	ASSERT_SUCCESS(device_finalize_machines(devices[1]) == NULL);
	ASSERT_SUCCESS(device_finalize_machines(devices[2]) == NULL);
	ASSERT_SUCCESS(device_finalize_machines(devices[3]) == NULL);

	deadline = devices[0]->machines->deadline[0][0] - 5000;
	ASSERT_EQ(deadline >= 100 && deadline <= 1000);
	ASSERT_EQ(deadline == devices[1]->machines->deadline[0][0] - 5000);
	ASSERT_EQ(deadline != devices[2]->machines->deadline[0][0] - 5000);
	ASSERT_EQ(deadline != devices[3]->machines->deadline[0][0] - 5000);

	device_tick(devices[0], 5000 + deadline - 1);
	ASSERT_EQ(devices[0]->machines->current[0] == 0);
	device_tick(devices[0], 5000 + deadline);
	ASSERT_EQ(devices[0]->machines->current[0] == 1);
	arena_release(arena);
}

TEST(MachineFiringCapStopsTickAndWarnsOnce)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "spinner");
	u16 tick_count = 0;

	add_state(def, "a");
	add_state(def, "b");
	tick_count = add_num_var(def, "ticks", 0);
	set_var_stmt(def, tick_count, node_binary(def, EXPR_ADD, node_var(def, tick_count),
						 node_num(def, 1)));
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	add_after(def, 0, 1, 1);
	add_after(def, 1, 0, 1);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 1000);
	ASSERT_EQ(dev->machines->current[0] == 0); /* 64 even firings */
	ASSERT_EQ(dev->machines->entered_at[0] == 1000);
	ASSERT_EQ(dev->machines->advanced_to[0] == 1000);
	ASSERT_EQ(dev->machines->deadline[0][0] == 1001);
	ASSERT_EQ(num_var_value(dev, 0, tick_count) == 0);
	ASSERT_EQ(dev->machines->cap_warned[0]);
	device_tick(dev, 2000);
	ASSERT_EQ(dev->machines->cap_warned[0]);
	ASSERT_EQ(dev->machines->entered_at[0] == 2000);
	arena_release(arena);
}

TEST(MachineBudgetExhaustionAbortsRemainingStatements)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "budget");
	u16 vars[5];
	u16 chain = 0;
	u16 first = 0;

	add_state(def, "s");
	for (u32 i = 0; i < 5; i++)
		vars[i] = add_num_var(def, "v", 0);
	chain = def->node_count;
	for (u32 i = 0; i < 150; i++)
	{
		u16 add = def->node_count++;
		u16 one = def->node_count++;
		u16 next = i == 149 ? (u16)(chain + 299) : (u16)(chain + 2 * (i + 1));

		def->nodes[add] = plain_node(EXPR_ADD);
		def->nodes[add].a = one;
		def->nodes[add].b = next;
		def->nodes[one] = plain_node(EXPR_NUM);
		def->nodes[one].literal = (val_t){ .type = VAL_NUM, .n = 1 };
	}
	first = def->statement_count;
	for (u32 i = 0; i < 4; i++)
		set_var_stmt(def, vars[i], chain);
	set_var_stmt(def, vars[4], node_num(def, 9));
	def->states[0].on_tick_first = first;
	def->states[0].on_tick_count = 5;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 100);
	ASSERT_EQ(num_var_value(dev, 0, vars[0]) == 151);
	ASSERT_EQ(num_var_value(dev, 0, vars[1]) == 151);
	ASSERT_EQ(num_var_value(dev, 0, vars[2]) == 151);
	ASSERT_EQ(num_var_value(dev, 0, vars[3]) == 0);
	ASSERT_EQ(num_var_value(dev, 0, vars[4]) == 0);
	ASSERT_EQ(dev->machines->warned[0][0][1] & 2); /* budget warning once */
	arena_release(arena);
}

TEST(MachineDivisionByZeroAbortsHandler)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "divide");
	u16 numerator = 0;
	u16 denominator = 0;
	u16 quotient = 0;
	u16 seven = 0;
	u16 nine = 0;
	u16 vars[3];

	add_state(def, "s");
	for (u32 i = 0; i < 3; i++)
		vars[i] = add_num_var(def, "v", 0);
	numerator = node_num(def, 1);
	denominator = node_num(def, 0);
	quotient = node_binary(def, EXPR_DIV, numerator, denominator);
	seven = node_num(def, 7);
	nine = node_num(def, 9);
	set_var_stmt(def, vars[0], seven);
	set_var_stmt(def, vars[1], quotient);
	set_var_stmt(def, vars[2], nine);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 3;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 1);
	ASSERT_EQ(num_var_value(dev, 0, vars[0]) == 7);
	ASSERT_EQ(num_var_value(dev, 0, vars[1]) == 0);
	ASSERT_EQ(num_var_value(dev, 0, vars[2]) == 0);
	ASSERT_EQ(dev->machines->warned[0][0][1] & 4); /* division warning */
	arena_release(arena);
}

TEST(MachineAbortedGuardSkipsBody)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* when_dev = device_new(arena, 0);
	device_t* unless_dev = device_new(arena, 0);
	machine_def_t* when_def = new_def(arena, "when-conditions");
	machine_def_t* unless_def = new_def(arena, "unless-conditions");
	u16 when_vars[3];
	u16 unless_vars[3];
	u16 bad = 0;
	u16 two = 0;
	u16 three = 0;
	u16 four = 0;
	u16 child = 0;

	add_state(when_def, "s");
	bad = node_binary(when_def, EXPR_GT,
			  node_binary(when_def, EXPR_DIV, node_num(when_def, 1), node_num(when_def, 0)),
			  node_num(when_def, 0));
	two = node_num(when_def, 2);
	three = node_num(when_def, 3);
	when_vars[0] = add_num_var(when_def, "child", 0);
	when_vars[1] = add_num_var(when_def, "after", 0);
	when_vars[2] = add_num_var(when_def, "before", 0);
	child = set_var_stmt(when_def, when_vars[0], three); /* guarded body, outside the handler range */
	set_var_stmt(when_def, when_vars[2], two);
	guard_stmt(when_def, STMT_WHEN, bad, child, 1);
	set_var_stmt(when_def, when_vars[1], three);
	when_def->states[0].on_tick_first = 1;
	when_def->states[0].on_tick_count = 3;

	add_state(unless_def, "s");
	bad = node_binary(unless_def, EXPR_GT,
			  node_binary(unless_def, EXPR_DIV, node_num(unless_def, 1), node_num(unless_def, 0)),
			  node_num(unless_def, 0));
	two = node_num(unless_def, 2);
	three = node_num(unless_def, 3);
	four = node_num(unless_def, 4);
	unless_vars[0] = add_num_var(unless_def, "child", 0);
	unless_vars[1] = add_num_var(unless_def, "after", 0);
	unless_vars[2] = add_num_var(unless_def, "before", 0);
	child = set_var_stmt(unless_def, unless_vars[0], three); /* guarded body */
	set_var_stmt(unless_def, unless_vars[2], two);
	guard_stmt(unless_def, STMT_UNLESS, bad, child, 1);
	set_var_stmt(unless_def, unless_vars[1], four);
	unless_def->states[0].on_tick_first = 1;
	unless_def->states[0].on_tick_count = 3;

	ASSERT_SUCCESS(device_add_machine(when_dev, when_def) == NULL);
	ASSERT_SUCCESS(device_add_machine(unless_dev, unless_def) == NULL);
	prepare(when_dev, 0);
	prepare(unless_dev, 0);

	device_tick(when_dev, 1);
	ASSERT_EQ(num_var_value(when_dev, 0, when_vars[2]) == 2); /* earlier effect kept */
	ASSERT_EQ(num_var_value(when_dev, 0, when_vars[0]) == 0); /* WHEN body skipped */
	ASSERT_EQ(num_var_value(when_dev, 0, when_vars[1]) == 0); /* handler aborted */
	ASSERT_EQ(when_dev->machines->warned[0][0][1] & 4); /* division warning */
	device_tick(unless_dev, 1);
	ASSERT_EQ(num_var_value(unless_dev, 0, unless_vars[2]) == 2); /* earlier effect kept */
	ASSERT_EQ(num_var_value(unless_dev, 0, unless_vars[0]) == 0); /* aborted UNLESS skips body */
	ASSERT_EQ(num_var_value(unless_dev, 0, unless_vars[1]) == 0); /* handler aborted */
	ASSERT_EQ(unless_dev->machines->warned[0][0][1] & 4); /* division warning */
	arena_release(arena);
}

TEST(MachineFailedPointAccessAbortsHandler)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "missing-point");
	u16 vars[3];

	add_state(def, "s");
	for (u32 i = 0; i < 3; i++)
		vars[i] = add_num_var(def, "v", 0);
	set_var_stmt(def, vars[0], node_num(def, 7));
	set_var_stmt(def, vars[1], node_point(def, SPACE_HOLDING, 999));
	set_var_stmt(def, vars[2], node_num(def, 9));
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 3;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 1);
	ASSERT_EQ(num_var_value(dev, 0, vars[0]) == 7);
	ASSERT_EQ(num_var_value(dev, 0, vars[1]) == 0);
	ASSERT_EQ(num_var_value(dev, 0, vars[2]) == 0);
	ASSERT_EQ(dev->machines->warned[0][0][1] & 8); /* point warning */
	arena_release(arena);
}

TEST(MachineRejectsRampTarget)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "ramp-writer");
	point_t ramp = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	const char* error = NULL;

	ramp.kind = POINT_RAMP;
	ramp.ramp = (ramp_params_t){ .from = 0, .to = 10, .step = 1,
				     .every_ms = 100, .mode = RAMP_HOLD };
	ASSERT_SUCCESS(device_add_point(dev, &ramp) == NULL);
	add_state(def, "s");
	set_point_stmt(def, SPACE_HOLDING, 0, node_num(def, 1));
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	device_seed_machines(dev, 0);
	error = device_finalize_machines(dev);
	ASSERT_FAILURE(error == NULL);
	ASSERT_SUCCESS(strstr(error, "ramp") != NULL);
	arena_release(arena);
}

TEST(MachineOwnershipAllowsOneMachineManyStatesButNotTwoMachines)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* shared = device_new(arena, 0);
	device_t* conflict = device_new(arena, 0);
	machine_def_t* one = new_def(arena, "one");
	machine_def_t* two = new_def(arena, "two");
	point_t point = static_point(SPACE_HOLDING, 4, VT_U16, 0);
	const char* error = NULL;

	add_state(one, "a");
	add_state(one, "b");
	set_point_stmt(one, SPACE_HOLDING, 4, node_num(one, 1));
	one->states[0].on_enter_first = 0;
	one->states[0].on_enter_count = 1;
	set_point_stmt(one, SPACE_HOLDING, 4, node_num(one, 2));
	one->states[1].on_tick_first = 1;
	one->states[1].on_tick_count = 1;
	add_state(two, "s");
	set_point_stmt(two, SPACE_HOLDING, 4, node_num(two, 3));
	two->states[0].on_tick_first = 0;
	two->states[0].on_tick_count = 1;

	ASSERT_SUCCESS(device_add_point(shared, &point) == NULL);
	ASSERT_SUCCESS(device_add_point(conflict, &point) == NULL);
	ASSERT_SUCCESS(device_add_machine(shared, one) == NULL);
	prepare(shared, 0);
	ASSERT_EQ(shared->points[0].kind == POINT_MACHINE);

	ASSERT_SUCCESS(device_add_machine(conflict, one) == NULL);
	ASSERT_SUCCESS(device_add_machine(conflict, two) == NULL);
	device_seed_machines(conflict, 0);
	error = device_finalize_machines(conflict);
	ASSERT_FAILURE(error == NULL);
	ASSERT_SUCCESS(strstr(error, "another machine") != NULL);
	ASSERT_EQ(conflict->points[0].kind == POINT_STATIC);
	arena_release(arena);
}

TEST(MachineValidatesTriggersAndAfterBounds)
{
	arena_t* arena = ARENA_ALLOC();
	machine_def_t* def = NULL;
	machine_transition_def_t* t = NULL;

	def = simple_def(arena);
	add_state(def, "next");
	add_transition(def, 0, 1);
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	t = add_transition(def, 0, 1);
	t->after_kind = AFTER_LITERAL;
	t->after_literal_ms = 0;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	t = add_transition(def, 0, 1);
	t->after_kind = AFTER_LITERAL;
	t->after_literal_ms = AFTER_MAX_MS + 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	add_random(def, 0, 1, 0, 10);
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	add_random(def, 0, 1, 100, 99);
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	add_random(def, 0, 1, 100, AFTER_MAX_MS + 1);
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	add_after(def, 2, 1, 10);
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	add_after(def, 0, 2, 10);
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	t = add_transition(def, 0, 1);
	t->has_when = true;
	t->when_root = 99;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_state(def, "next");
	t = add_transition(def, 0, 1);
	t->has_when = true;
	t->when_root = node_num(def, 1);
	expect_def_error(arena, def);
	arena_release(arena);
}

TEST(MachineValidatesNamesCountsAndStatementRanges)
{
	arena_t* arena = ARENA_ALLOC();
	machine_def_t* def = NULL;

	def = new_def(arena, "");
	def->states[0].name[0] = 's';
	def->state_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->states[1].name[0] = 's';
	def->state_count = 2;
	def->states[1].name[1] = '\0';
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->state_count = 0;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->initial_state = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->states[0].name[0] = '\0';
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->vars[0].name[0] = '\0';
	def->var_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->vars[0].name[0] = 'v';
	def->vars[1].name[0] = 'v';
	def->var_count = 2;
	expect_def_error(arena, def);

	def = simple_def(arena);
	set_var_stmt(def, 3, node_num(def, 1));
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->states[0].on_tick_first = 1;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	guard_stmt(def, STMT_WHEN, node_bool(def, true), 0, 2);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	guard_stmt(def, STMT_WHEN, node_bool(def, true), 0, 1);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def); /* statement cycle */

	def = simple_def(arena);
	def->var_count = MACHINE_MAX_VARS + 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->state_count = MACHINE_MAX_STATES + 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->transition_count = MACHINE_MAX_TRANSITIONS + 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->statement_count = MACHINE_MAX_STATEMENTS + 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	def->node_count = MACHINE_MAX_EXPR_NODES + 1;
	expect_def_error(arena, def);
	arena_release(arena);
}

TEST(MachineValidatesExpressionTypesAndCycles)
{
	arena_t* arena = ARENA_ALLOC();
	machine_def_t* def = NULL;
	u16 root = 0;

	def = simple_def(arena);
	set_var_stmt(def, 0, 99); /* no var, but out-of-range expression root first */
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_binary(def, EXPR_ADD, 1, 1);
	def->nodes[root].b = 99;
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_binary(def, EXPR_ADD, 0, 1);
	def->nodes[1] = plain_node(EXPR_NUM);
	def->nodes[1].literal = (val_t){ .type = VAL_NUM, .n = 1 };
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def); /* expression cycle */

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = def->node_count++;
	def->nodes[root] = plain_node((expr_kind_t)999);
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_binary(def, EXPR_ADD, node_bool(def, true), node_num(def, 1));
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_binary(def, EXPR_EQ, node_bool(def, true), node_num(def, 1));
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_if(def, node_bool(def, true), node_bool(def, false), node_num(def, 1));
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def); /* literal IF branches */

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	add_bool_var(def, "b", true);
	root = node_if(def, node_bool(def, true), node_var(def, 1), node_var(def, 0));
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def); /* VAR IF branches */

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_if(def, node_bool(def, true),
		       node_point(def, SPACE_COIL, 1), node_point(def, SPACE_HOLDING, 1));
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def); /* POINT IF branches */

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_if(def, node_num(def, 1), node_num(def, 1), node_num(def, 2));
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def); /* non-bool IF condition */

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_var(def, 5);
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	root = node_point(def, (space_kind_t)SPACE_COUNT, 0);
	set_var_stmt(def, 0, root);
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	add_num_var(def, "n", 0);
	set_var_stmt(def, 0, node_bool(def, true)); /* bool value into num var */
	def->states[0].on_tick_first = 0;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def);

	def = simple_def(arena);
	u16 guard = guard_stmt(def, STMT_WHEN, node_num(def, 1), 0, 0);

	def->states[0].on_tick_first = guard;
	def->states[0].on_tick_count = 1;
	expect_def_error(arena, def); /* non-bool condition */
	arena_release(arena);
}

TEST(MachineRejectsDuplicateAndExcessMachines)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* duplicate = device_new(arena, 0);
	device_t* excess = device_new(arena, 0);
	machine_def_t* def = simple_def(arena);

	ASSERT_SUCCESS(device_add_machine(duplicate, def) == NULL);
	ASSERT_SUCCESS(device_add_machine(duplicate, def) == NULL);
	device_seed_machines(duplicate, 0);
	ASSERT_FAILURE(device_finalize_machines(duplicate) == NULL);

	for (u32 i = 0; i < MACHINE_MAX_MACHINES; i++)
	{
		machine_def_t* copy = ARENA_PUSH_STRUCT(arena, machine_def_t);

		*copy = *def;
		snprintf(copy->name, sizeof(copy->name), "m%u", i);
		ASSERT_SUCCESS(device_add_machine(excess, copy) == NULL);
	}
	ASSERT_SUCCESS(device_add_machine(excess, def) != NULL);
	device_seed_machines(excess, 0);
	ASSERT_SUCCESS(device_finalize_machines(excess) == NULL);
	ASSERT_FAILURE(device_add_machine(excess, def) == NULL);
	arena_release(arena);
}

TEST(DeviceWithoutMachinesTicksNormally)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);

	ASSERT_EQ(dev->machines == NULL);
	ASSERT_SUCCESS(device_finalize_machines(dev) == NULL);
	ASSERT_EQ(dev->machines == NULL);
	device_tick(dev, 100);
	ASSERT_EQ(dev->machines == NULL);

	ASSERT_SUCCESS(device_add_machine(dev, simple_def(arena)) == NULL);
	ASSERT_SUCCESS(dev->machines != NULL);
	ASSERT_EQ(dev->machines->count == 1);
	arena_release(arena);
}

TEST(MachineOnWriteRunsInSameRequestWithoutTransition)
{
	enum { V_SEEN, V_COUNT };
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 1000);
	machine_def_t* def = new_def(arena, "writer");
	point_t watched = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	u16 statement = 0;
	u16 word = 7;

	ASSERT_SUCCESS(device_add_point(dev, &watched) == NULL);
	add_state(def, "idle");
	add_state(def, "done");
	add_num_var(def, "seen", 0);
	add_when(def, 0, 1, node_bool(def, true));
	statement = set_var_stmt(def, V_SEEN,
				 node_binary(def, EXPR_ADD, node_point(def, SPACE_HOLDING, 0),
					     node_num(def, 1)));
	def->write_handlers[0] = (machine_write_handler_t){
		.first = statement, .count = 1, .space = SPACE_HOLDING, .address = 0,
	};
	def->write_handler_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	ASSERT_EQ(num_var_value(dev, 0, V_SEEN) == 0);
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, &word));
	ASSERT_EQ(num_var_value(dev, 0, V_SEEN) == 8); /* sees the just-written value */
	ASSERT_EQ(dev->machines->current[0] == 0);
	ASSERT_EQ(dev->machines->advanced_to[0] == 1000); /* no tick happened */

	device_tick(dev, 1100);
	ASSERT_EQ(dev->machines->current[0] == 1);
	arena_release(arena);
}

TEST(MachineOnWriteWatchesEveryWordOfPoint)
{
	enum { V_SEEN, V_COUNT };
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "wide");
	point_t watched = static_point(SPACE_HOLDING, 0, VT_U32, 0);
	u16 statement = 0;
	u16 word = 1;

	ASSERT_SUCCESS(device_add_point(dev, &watched) == NULL);
	add_state(def, "idle");
	add_num_var(def, "seen", 0);
	statement = set_var_stmt(def, V_SEEN, node_point(def, SPACE_HOLDING, 0));
	def->write_handlers[0] = (machine_write_handler_t){
		.first = statement, .count = 1, .space = SPACE_HOLDING, .address = 0,
	};
	def->write_handler_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 1, 1, &word));
	ASSERT_EQ(num_var_value(dev, 0, V_SEEN) == 1);
	arena_release(arena);
}

TEST(MachineStateRegisterTracksTransitionsAndIsReadOnly)
{
	enum { V_SEEN, V_COUNT };
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 1000);
	machine_def_t* def = new_def(arena, "state-view");
	point_t register_point = static_point(SPACE_HOLDING, 1, VT_U16, 0);
	u16 word = 42;
	u16 out = 0;

	ASSERT_SUCCESS(device_add_point(dev, &register_point) == NULL);
	def->has_state_register = true;
	def->state_register_space = SPACE_HOLDING;
	def->state_register_address = 1;
	add_state(def, "idle");
	add_state(def, "done");
	add_num_var(def, "seen", 0);
	add_after(def, 0, 1, 100);
	def->states[1].on_enter_first = set_var_stmt(def, V_SEEN,
						    node_point(def, SPACE_HOLDING, 1));
	def->states[1].on_enter_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 0);
	device_tick(dev, 1100);
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 1);
	ASSERT_EQ(num_var_value(dev, 0, V_SEEN) == 1);
	ASSERT_EQ(dev->points[0].kind == POINT_MACHINE);
	ASSERT_FAILURE(device_write(dev, SPACE_HOLDING, 1, 1, &word));
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 1);
	arena_release(arena);
}

TEST(EffectiveFaultsFollowActiveStateAndFirstMachineWins)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* first = new_def(arena, "first");
	machine_def_t* second = new_def(arena, "second");
	fault_set_t effective = { 0 };

	dev->faults.by_kind[FAULT_DELAY] = (fault_t){ .delay_ms = 100, .percent = 100 };
	dev->faults.present |= FAULT_BIT(FAULT_DELAY);
	add_state(first, "idle");
	add_state(first, "done");
	add_after(first, 0, 1, 100);
	first->states[0].faults.by_kind[FAULT_DROP] = (fault_t){ .percent = 100 };
	first->states[0].faults.present |= FAULT_BIT(FAULT_DROP);
	first->states[1].faults.by_kind[FAULT_EXCEPTION] = (fault_t){
		.exception_code = 4, .percent = 100,
	};
	first->states[1].faults.present |= FAULT_BIT(FAULT_EXCEPTION);
	add_state(second, "idle");
	second->states[0].faults.by_kind[FAULT_DELAY] = (fault_t){ .delay_ms = 5, .percent = 100 };
	second->states[0].faults.present |= FAULT_BIT(FAULT_DELAY);
	second->states[0].faults.by_kind[FAULT_DROP] = (fault_t){ .percent = 0 };
	second->states[0].faults.present |= FAULT_BIT(FAULT_DROP);
	ASSERT_SUCCESS(device_add_machine(dev, first) == NULL);
	ASSERT_SUCCESS(device_add_machine(dev, second) == NULL);
	prepare(dev, 0);

	effective = device_effective_faults(dev);
	ASSERT_EQ(effective.by_kind[FAULT_DELAY].delay_ms == 5);
	ASSERT_EQ(effective.by_kind[FAULT_DROP].percent == 100);
	ASSERT_EQ((effective.present & FAULT_BIT(FAULT_EXCEPTION)) == 0);

	device_tick(dev, 100);
	effective = device_effective_faults(dev);
	ASSERT_EQ(effective.by_kind[FAULT_DELAY].delay_ms == 5);
	ASSERT_EQ(effective.by_kind[FAULT_DROP].percent == 0); /* second machine's overlay now applies */
	ASSERT_EQ(effective.by_kind[FAULT_EXCEPTION].exception_code == 4);
	ASSERT_EQ(device_machine_fault_present(dev, FAULT_DISCONNECT) == false);
	arena_release(arena);
}

TEST(MachineOnWriteRunsMachinesInDeclarationOrder)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* first = new_def(arena, "first");
	machine_def_t* second = new_def(arena, "second");
	point_t watched0 = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	point_t watched1 = static_point(SPACE_HOLDING, 1, VT_U16, 0);
	point_t shared = static_point(SPACE_HOLDING, 2, VT_U16, 0);
	u16 words[2] = { 1, 2 };
	u16 out = 0;
	u16 stmt = 0;

	ASSERT_SUCCESS(device_add_point(dev, &watched0) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &watched1) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &shared) == NULL);

	add_state(first, "idle");
	add_num_var(first, "seen", 0);
	stmt = set_point_stmt(first, SPACE_HOLDING, 2, node_num(first, 7));
	first->write_handlers[0] = (machine_write_handler_t){
		.first = stmt, .count = 1, .space = SPACE_HOLDING, .address = 0,
	};
	first->write_handler_count = 1;

	add_state(second, "idle");
	add_num_var(second, "seen", 0);
	stmt = set_var_stmt(second, 0, node_point(second, SPACE_HOLDING, 2));
	second->write_handlers[0] = (machine_write_handler_t){
		.first = stmt, .count = 1, .space = SPACE_HOLDING, .address = 1,
	};
	second->write_handler_count = 1;

	ASSERT_SUCCESS(device_add_machine(dev, first) == NULL);
	ASSERT_SUCCESS(device_add_machine(dev, second) == NULL);
	prepare(dev, 0);

	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 2, words));
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 2, 1, &out));
	ASSERT_EQ(out == 7); /* first machine ran before the second read the point */
	ASSERT_EQ(num_var_value(dev, 1, 0) == 7);
	arena_release(arena);
}

TEST(MachineOnWriteWarningsUseCurrentState)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "writer");
	point_t watched = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	u16 word = 1;
	u16 stmt = 0;

	ASSERT_SUCCESS(device_add_point(dev, &watched) == NULL);
	add_state(def, "idle");
	add_state(def, "running");
	def->initial_state = 1;
	add_num_var(def, "seen", 0);
	stmt = set_var_stmt(def, 0, node_binary(def, EXPR_DIV, node_num(def, 1), node_num(def, 0)));
	def->write_handlers[0] = (machine_write_handler_t){
		.first = stmt, .count = 1, .space = SPACE_HOLDING, .address = 0,
	};
	def->write_handler_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, &word));
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_EQ((dev->machines->warned[0][1][MACHINE_ROLE_WRITE] & 4) != 0);
	ASSERT_EQ(dev->machines->warned[0][0][MACHINE_ROLE_WRITE] == 0);
	ASSERT_EQ(dev->machines->warned[0][1][MACHINE_ROLE_TICK] == 0);
	ASSERT_EQ(num_var_value(dev, 0, 0) == 0);
	arena_release(arena);
}

TEST(MachineOnWriteUsesRequestTickTime)
{
	enum { V_NOW, V_ELAPSED, V_COUNT };
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 1000);
	machine_def_t* def = new_def(arena, "clock");
	point_t watched = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	u16 word = 1;
	u16 first = 0;

	ASSERT_SUCCESS(device_add_point(dev, &watched) == NULL);
	add_state(def, "idle");
	add_num_var(def, "now", 0);
	add_num_var(def, "elapsed", 0);
	first = def->statement_count;
	set_var_stmt(def, V_NOW, node_time(def, false));
	set_var_stmt(def, V_ELAPSED, node_time(def, true));
	def->write_handlers[0] = (machine_write_handler_t){
		.first = first, .count = 2, .space = SPACE_HOLDING, .address = 0,
	};
	def->write_handler_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	device_tick(dev, 2000);
	device_tick(dev, 5000);
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, &word));
	ASSERT_EQ(num_var_value(dev, 0, V_NOW) == 5000);
	ASSERT_EQ(num_var_value(dev, 0, V_ELAPSED) == 0); /* elapsed counts from the same tick */
	arena_release(arena);
}

TEST(MachineStateRegisterInitializesBeforeInitialOnEnter)
{
	enum { V_SEEN, V_COUNT };
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "state-view");
	point_t register_point = static_point(SPACE_INPUT, 3, VT_U16, 0);
	u16 out = 0;

	ASSERT_SUCCESS(device_add_point(dev, &register_point) == NULL);
	def->has_state_register = true;
	def->state_register_space = SPACE_INPUT;
	def->state_register_address = 3;
	add_state(def, "idle");
	add_state(def, "running");
	def->initial_state = 1; /* zero-based state index, not state name order */
	add_num_var(def, "seen", 0);
	def->states[0].on_enter_first = set_var_stmt(def, V_SEEN,
						node_point(def, SPACE_INPUT, 3));
	def->states[0].on_enter_count = 1;
	def->states[1].on_enter_first = set_var_stmt(def, V_SEEN,
						node_point(def, SPACE_INPUT, 3));
	def->states[1].on_enter_count = 1;
	add_after(def, 1, 0, 10);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 3, 1, &out));
	ASSERT_EQ(out == 1);
	ASSERT_EQ(num_var_value(dev, 0, V_SEEN) == 1);

	device_tick(dev, 10);
	ASSERT_EQ(dev->machines->current[0] == 0);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 3, 1, &out));
	ASSERT_EQ(out == 0);
	ASSERT_EQ(num_var_value(dev, 0, V_SEEN) == 0);
	arena_release(arena);
}

TEST(MachineFinalizeRejectsStateRegisterSetTarget)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "conflict");
	point_t register_point = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	const char* error = NULL;

	ASSERT_SUCCESS(device_add_point(dev, &register_point) == NULL);
	def->has_state_register = true;
	def->state_register_space = SPACE_HOLDING;
	def->state_register_address = 0;
	add_state(def, "idle");
	def->states[0].on_enter_first = set_point_stmt(def, SPACE_HOLDING, 0, node_num(def, 1));
	def->states[0].on_enter_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	device_seed_machines(dev, 0);
	error = device_finalize_machines(dev);
	ASSERT_FAILURE(error == NULL);
	ASSERT_EQ(strstr(error, "state register cannot be driven by a machine") != NULL);
	arena_release(arena);
}

TEST(DeviceRestartResetsMachineStateVarsTimersAndKeepsRng)
{
	enum { V_COUNTER };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev = device_new(arena, 1000);
	machine_def_t*	def = new_def(arena, "restartable");
	point_t		state_reg = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	point_t		output = static_point(SPACE_HOLDING, 1, VT_U16, 0);
	rng_t		rng_before;
	u16		out = 0;
	u16		stmt = 0;

	ASSERT_SUCCESS(device_add_point(dev, &state_reg) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &output) == NULL);
	def->has_state_register = true;
	def->state_register_space = SPACE_HOLDING;
	def->state_register_address = 0;
	add_state(def, "idle");
	add_state(def, "running");
	add_num_var(def, "counter", 0);

	stmt = set_var_stmt(def, V_COUNTER, node_num(def, 7));
	def->states[0].on_enter_first = stmt;
	def->states[0].on_enter_count = 2;
	(void)set_point_stmt(def, SPACE_HOLDING, 1, node_num(def, 11));

	stmt = set_var_stmt(def, V_COUNTER, node_num(def, 55));
	def->states[1].on_enter_first = stmt;
	def->states[1].on_enter_count = 2;
	(void)set_point_stmt(def, SPACE_HOLDING, 1, node_num(def, 22));
	add_after(def, 0, 1, 1000);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 42);

	ASSERT_EQ(dev->machines->current[0] == 0);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 0);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 11);
	ASSERT_EQ(num_var_value(dev, 0, V_COUNTER) == 7);

	device_tick(dev, 2000);
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 1);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 22);
	ASSERT_EQ(num_var_value(dev, 0, V_COUNTER) == 55);

	rng_before = dev->machines->rng;
	device_restart(dev, 5000);
	ASSERT_EQ(dev->start_ms == 5000 && dev->now_ms == 5000);
	ASSERT_EQ(dev->machines->current[0] == 0);
	ASSERT_EQ(dev->machines->entered_at[0] == 5000);
	ASSERT_EQ(dev->machines->advanced_to[0] == 5000);
	ASSERT_EQ(memcmp(&rng_before, &dev->machines->rng, sizeof(rng_before)) == 0);

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 0);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &out));
	ASSERT_EQ(out == 11);
	ASSERT_EQ(num_var_value(dev, 0, V_COUNTER) == 7);
	ASSERT_EQ(dev->points[0].kind == POINT_MACHINE);
	ASSERT_EQ(dev->points[1].kind == POINT_MACHINE);

	device_tick(dev, 6000);
	ASSERT_EQ(dev->machines->current[0] == 1);
	ASSERT_EQ(dev->machines->entered_at[0] == 6000);
	arena_release(arena);
}

TEST(EffectiveFaultsKeepUnmentionedDeviceFaults)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	machine_def_t* def = new_def(arena, "overlay");
	fault_set_t effective = { 0 };

	dev->faults.by_kind[FAULT_DELAY] = (fault_t){ .delay_ms = 25, .percent = 100 };
	dev->faults.present |= FAULT_BIT(FAULT_DELAY);
	dev->faults.by_kind[FAULT_EXCEPTION] = (fault_t){ .exception_code = 8, .percent = 100 };
	dev->faults.present |= FAULT_BIT(FAULT_EXCEPTION);
	add_state(def, "idle");
	def->states[0].faults.by_kind[FAULT_DROP] = (fault_t){ .percent = 100 };
	def->states[0].faults.present |= FAULT_BIT(FAULT_DROP);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	prepare(dev, 0);

	effective = device_effective_faults(dev);
	ASSERT_EQ(effective.by_kind[FAULT_DELAY].delay_ms == 25);
	ASSERT_EQ(effective.by_kind[FAULT_EXCEPTION].exception_code == 8);
	ASSERT_EQ(effective.by_kind[FAULT_DROP].percent == 100);
	ASSERT_EQ(effective.present == (FAULT_BIT(FAULT_DELAY) |
				       FAULT_BIT(FAULT_EXCEPTION) |
				       FAULT_BIT(FAULT_DROP)));
	arena_release(arena);
}
