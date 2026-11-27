#include "vtest.h"
#include "device/expr.h"
#include <math.h>

static expr_node_t
node(expr_kind_t kind)
{
	return (expr_node_t){
		.kind = kind,
		.a = EXPR_NO_CHILD,
		.b = EXPR_NO_CHILD,
		.c = EXPR_NO_CHILD,
	};
}

static expr_node_t
num(f64 value)
{
	expr_node_t n = node(EXPR_NUM);

	n.literal = (val_t){ .type = VAL_NUM, .n = value };
	return n;
}

static expr_node_t
boolean(bool value)
{
	expr_node_t n = node(EXPR_BOOL);

	n.literal = (val_t){ .type = VAL_BOOL, .b = value };
	return n;
}

static expr_node_t
binary(expr_kind_t kind, u16 left, u16 right)
{
	expr_node_t n = node(kind);

	n.a = left;
	n.b = right;
	return n;
}

static expr_node_t
conditional(u16 cond, u16 then_index, u16 else_index)
{
	expr_node_t n = node(EXPR_IF);

	n.a = cond;
	n.b = then_index;
	n.c = else_index;
	return n;
}

static expr_node_t
variable(u16 index)
{
	expr_node_t n = node(EXPR_VAR);

	n.a = index;
	return n;
}

static expr_node_t
point(space_kind_t space, u32 address)
{
	expr_node_t n = node(EXPR_POINT);

	n.space = (u8)space;
	n.address = address;
	return n;
}

static expr_ctx_t
context(val_t* vars, u32 budget)
{
	return (expr_ctx_t){
		.dev = NULL,
		.vars = vars,
		.now_ms = 1234,
		.elapsed_ms = 56,
		.budget = budget,
	};
}

static void
expect_failure(const expr_node_t* nodes, u16 root, expr_ctx_t* ctx)
{
	val_t out = { .type = VAL_NUM, .n = 99 };

	ctx->budget = 16;
	ASSERT_FAILURE(expr_eval(nodes, root, ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 99);
}

TEST(ExprLiteralsKeepTypes)
{
	expr_node_t nodes[] = { boolean(true), num(2.5) };
	expr_ctx_t ctx = context(NULL, 1);
	val_t out = { 0 };

	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b && ctx.budget == 0);
	ctx.budget = 1;
	ASSERT_SUCCESS(expr_eval(nodes, 1, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 2.5 && ctx.budget == 0);

	out = (val_t){ .type = VAL_BOOL, .b = false };
	ctx.budget = 0;
	ASSERT_FAILURE(expr_eval(nodes, 1, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b && ctx.budget == 0);
}

TEST(ExprReadsVarsAndTime)
{
	val_t vars[] = {
		{ .type = VAL_BOOL, .b = false },
		{ .type = VAL_NUM, .n = 12.5 },
	};
	expr_node_t nodes[] = {
		variable(0), variable(1), node(EXPR_NOW_MS), node(EXPR_ELAPSED_MS),
	};
	expr_ctx_t ctx = context(vars, 8);
	val_t out = { 0 };

	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 1, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 12.5);
	ASSERT_SUCCESS(expr_eval(nodes, 2, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 1234);
	ASSERT_SUCCESS(expr_eval(nodes, 3, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 56);
	ASSERT_EQ(ctx.budget == 4);
}

TEST(ExprReadsPointsAndPropagatesFailure)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = device_new(arena, 0);
	point_t holding = {
		.space = SPACE_HOLDING, .address = 10, .type = VT_U16,
		.kind = POINT_STATIC, .static_value = 7,
	};
	point_t input = {
		.space = SPACE_INPUT, .address = 5, .type = VT_F32,
		.order = WORDS_HIGH_FIRST, .kind = POINT_STATIC, .static_value = -2.5,
	};
	point_t coil = {
		.space = SPACE_COIL, .address = 3, .type = VT_BOOL,
		.kind = POINT_STATIC, .static_value = 1,
	};
	point_t discrete = {
		.space = SPACE_DISCRETE, .address = 2, .type = VT_BOOL,
		.kind = POINT_STATIC, .static_value = 0,
	};
	expr_node_t nodes[] = {
		point(SPACE_HOLDING, 10), point(SPACE_INPUT, 5),
		point(SPACE_COIL, 3), point(SPACE_DISCRETE, 2),
		point(SPACE_HOLDING, 99),
	};
	expr_ctx_t ctx = context(NULL, 8);
	val_t out = { 0 };

	ASSERT_SUCCESS(device_add_point(dev, &holding) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &input) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &coil) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &discrete) == NULL);
	ctx.dev = dev;

	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 7);
	ASSERT_SUCCESS(expr_eval(nodes, 1, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == -2.5);
	ASSERT_SUCCESS(expr_eval(nodes, 2, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 3, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
	ASSERT_EQ(ctx.budget == 4);
	expect_failure(nodes, 4, &ctx);
	arena_release(arena);
}

TEST(ExprArithmeticOperators)
{
	expr_node_t nodes[] = {
		binary(EXPR_ADD, 8, 9),		/* 2 + 3 */
		binary(EXPR_SUB, 8, 9),		/* 2 - 3 */
		binary(EXPR_MUL, 8, 9),		/* 2 * 3 */
		binary(EXPR_DIV, 10, 8),	/* 4 / 2 */
		binary(EXPR_MIN, 8, 11),
		binary(EXPR_MAX, 8, 11),
		binary(EXPR_MIN, 8, 12),	/* fmin(2, NaN) */
		binary(EXPR_MAX, 8, 12),	/* fmax(2, NaN) */
		num(2), num(3), num(4), num(1.5), num(NAN),
	};
	expr_ctx_t ctx = context(NULL, 32);
	val_t out = { 0 };

	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 5 && ctx.budget == 29);
	ASSERT_SUCCESS(expr_eval(nodes, 1, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == -1);
	ASSERT_SUCCESS(expr_eval(nodes, 2, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 6);
	ASSERT_SUCCESS(expr_eval(nodes, 3, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 2);
	ASSERT_SUCCESS(expr_eval(nodes, 4, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 1.5);
	ASSERT_SUCCESS(expr_eval(nodes, 5, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 2);
	ASSERT_SUCCESS(expr_eval(nodes, 6, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 2);
	ASSERT_SUCCESS(expr_eval(nodes, 7, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 2);
	ASSERT_EQ(ctx.budget == 8);
}

TEST(ExprComparisonOperators)
{
	expr_node_t nodes[] = {
		binary(EXPR_LT, 8, 9),
		binary(EXPR_LE, 8, 8),
		binary(EXPR_GT, 9, 8),
		binary(EXPR_GE, 9, 10),
		binary(EXPR_EQ, 8, 8),
		binary(EXPR_EQ, 8, 9),
		binary(EXPR_EQ, 11, 11),
		binary(EXPR_EQ, 11, 12),
		num(1), num(2), num(3), boolean(true), boolean(false),
	};
	expr_ctx_t ctx = context(NULL, 32);
	val_t out = { 0 };

	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 1, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 2, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 3, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 4, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 5, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 6, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 7, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
	ASSERT_EQ(ctx.budget == 32 - 8 * 3);
}

TEST(ExprBooleanOperators)
{
	expr_node_t nodes[] = {
		binary(EXPR_AND, 4, 4),
		binary(EXPR_AND, 4, 5),
		binary(EXPR_OR, 5, 4),
		binary(EXPR_OR, 5, 5),
		boolean(true), boolean(false),
		node(EXPR_NOT),
	};

	expr_ctx_t ctx = context(NULL, 16);
	val_t out = { 0 };

	nodes[6].a = 5;
	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 1, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 2, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 3, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
	ASSERT_SUCCESS(expr_eval(nodes, 6, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	nodes[6].a = 4;
	ASSERT_SUCCESS(expr_eval(nodes, 6, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b);
}

static void
expect_value(const expr_node_t* nodes, u16 root, u32 budget, val_type_t type, f64 want)
{
	expr_ctx_t	ctx = context(NULL, budget);
	val_t		out = { 0 };

	ASSERT_SUCCESS(expr_eval(nodes, root, &ctx, &out));
	ASSERT_EQ(out.type == type);
	ASSERT_EQ((type == VAL_BOOL ? (f64)out.b : out.n) == want);
	ASSERT_EQ(ctx.budget == 0);	/* only the evaluated nodes are charged */
}

TEST(ExprIfEvaluatesOnlyTheChosenBranch)
{
	expr_node_t nested[] = {
		conditional(1, 2, 3), boolean(true), num(10),
		conditional(4, 5, 6), boolean(false), num(20), num(30),
	};
	/* T4 must reject different IF branch types at load time, including
	 * VAR/POINT branches; the evaluator only checks the nodes it evaluates. */
	expr_node_t typed[] = {
		conditional(3, 1, 2), boolean(true), boolean(false), boolean(true),
		conditional(7, 5, 6), num(11), num(22), boolean(false),
	};

	expect_value(nested, 0, 3, VAL_NUM, 10);
	nested[1] = boolean(false);
	expect_value(nested, 0, 5, VAL_NUM, 30);
	nested[4] = boolean(true);
	expect_value(nested, 0, 5, VAL_NUM, 20);
	expect_value(typed, 0, 3, VAL_BOOL, 1);
	expect_value(typed, 4, 3, VAL_NUM, 22);
}

TEST(ExprSharedIfDagChargesOnlyEvaluatedNodes)
{
	enum { LEVELS = 40, COUNT = LEVELS + 2, COST = 2 * LEVELS + 1 };
	expr_node_t nodes[COUNT];
	expr_ctx_t ctx = context(NULL, 5);
	val_t out = { .type = VAL_NUM, .n = 99 };

	for (u32 i = 0; i < LEVELS; i++)
	{
		u16 next = i == LEVELS - 1 ? LEVELS + 1 : (u16)(i + 1);

		nodes[i] = conditional((u16)LEVELS, next, next);
	}
	nodes[LEVELS] = boolean(true);
	nodes[LEVELS + 1] = num(42);

	ASSERT_FAILURE(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 99 && ctx.budget == 0);

	ctx.budget = COST;
	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 42 && ctx.budget == 0);
}

TEST(ExprDepthGuardStopsCyclicGraph)
{
	expr_node_t nodes[] = { binary(EXPR_ADD, 0, 0) };
	expr_ctx_t ctx = context(NULL, 100000);
	val_t out = { .type = VAL_NUM, .n = 99 };

	ASSERT_FAILURE(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 99);
	ASSERT_EQ(ctx.budget < 100000);
}

TEST(ExprAndOrShortCircuit)
{
	expr_node_t nodes[] = {
		binary(EXPR_AND, 1, 2),	/* false && (1 / 0) */
		boolean(false),
		binary(EXPR_DIV, 3, 4),
		num(1),
		num(0),
		binary(EXPR_OR, 6, 2),	/* true || (1 / 0) */
		boolean(true),
	};
	expr_ctx_t ctx = context(NULL, 2);
	val_t out = { 0 };

	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b && ctx.budget == 0);

	ctx.budget = 2;
	ASSERT_SUCCESS(expr_eval(nodes, 5, &ctx, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b && ctx.budget == 0);

	ctx.budget = 1;
	ASSERT_FAILURE(expr_eval(nodes, 0, &ctx, &out));
}

TEST(ExprBudgetExhaustionLeavesOutputUntouched)
{
	enum { CHAIN = 64, COUNT = 2 * CHAIN + 1 };
	expr_node_t nodes[COUNT];
	expr_ctx_t ctx = context(NULL, CHAIN);
	val_t out = { .type = VAL_NUM, .n = 99 };

	for (u32 i = 0; i < CHAIN; i++)
	{
		u16 next = i == CHAIN - 1 ? COUNT - 1 : (u16)(2 * (i + 1));

		nodes[2 * i] = binary(EXPR_ADD, (u16)(2 * i + 1), next);
		nodes[2 * i + 1] = num(1);
	}
	nodes[COUNT - 1] = num(0);

	ASSERT_FAILURE(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 99 && ctx.budget == 0);

	ctx.budget = COUNT - 1;
	ASSERT_FAILURE(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 99 && ctx.budget == 0);

	ctx.budget = COUNT;
	ASSERT_SUCCESS(expr_eval(nodes, 0, &ctx, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == CHAIN && ctx.budget == 0);
}

TEST(ExprDivisionByZeroFails)
{
	expr_node_t nodes[] = {
		binary(EXPR_DIV, 2, 3),
		binary(EXPR_DIV, 2, 4),
		num(5), num(0), num(-0.0),
	};
	expr_ctx_t ctx = context(NULL, 8);

	expect_failure(nodes, 0, &ctx);
	expect_failure(nodes, 1, &ctx);
}

TEST(ExprRejectsIllTypedNodes)
{
	val_t vars[] = { { .type = VAL_BOOL, .b = true } };
	expr_ctx_t ctx = context(vars, 16);
	expr_node_t bad_literal = boolean(true);

	bad_literal.literal.type = VAL_NUM;
	{
		expr_node_t nodes[] = { binary(EXPR_ADD, 1, 2), num(1), boolean(true) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { binary(EXPR_ADD, 1, 2), boolean(true), num(1) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { binary(EXPR_LT, 1, 1), boolean(true) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { binary(EXPR_EQ, 1, 2), num(1), boolean(true) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { binary(EXPR_AND, 1, 2), num(1), boolean(true) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { binary(EXPR_AND, 1, 2), boolean(true), num(1) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { binary(EXPR_OR, 1, 2), num(1), boolean(true) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { binary(EXPR_OR, 1, 2), boolean(false), num(1) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { node(EXPR_NOT), num(1) };
		nodes[0].a = 1;
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { conditional(1, 2, 3), num(1), num(2), boolean(true) };
		expect_failure(nodes, 0, &ctx);
	}
	{
		expr_node_t nodes[] = { bad_literal };
		expect_failure(nodes, 0, &ctx);
	}
}
