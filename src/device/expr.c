/**
 * @file
 * @brief Evaluator for compiled machine expressions.
 */
#include "device/expr.h"
#include <math.h>

/** @brief Maximum nesting depth of an expression tree. */
enum { EXPR_MAX_DEPTH = 512 };

const char*
expr_failure_text(expr_failure_t why)
{
	static const char* const names[] = {
		"", "evaluation budget exhausted", "division by zero",
		"point access failed", "invalid expression", "integer operand out of range",
	};

	return (u32)why < sizeof(names) / sizeof(names[0]) ? names[why] : "";
}

static bool
fail(expr_ctx_t* ctx, expr_failure_t reason)
{
	ctx->failure = reason;
	return false;
}

/* 2^53: the largest bound at which every whole number is exact in an f64. */
#define EXPR_BIT_LIMIT 9007199254740992.0

static bool
whole_bits(f64 v, u64* bits)
{
	if (!(v >= 0.0) ||
	    v >= EXPR_BIT_LIMIT ||
	    v != floor(v))
		return false;
	*bits = (u64)v;
	return true;
}

static bool
eval_node(const expr_node_t* nodes, u16 index, expr_ctx_t* ctx, val_t* out, u16 depth)
{
	const expr_node_t* node = &nodes[index];
	val_t lhs;
	val_t rhs;
	val_t cond;
	val_t result;
	u16 chosen;

	if (depth >= EXPR_MAX_DEPTH)
		return fail(ctx, EXPR_FAIL_BUDGET);
	if (ctx->budget == 0)
		return fail(ctx, EXPR_FAIL_BUDGET);
	ctx->budget--;

	switch (node->kind)
	{
	case EXPR_BOOL:
		if (node->literal.type != VAL_BOOL)
			return fail(ctx, EXPR_FAIL_TYPE);
		*out = node->literal;
		return true;
	case EXPR_NUM:
		if (node->literal.type != VAL_NUM)
			return fail(ctx, EXPR_FAIL_TYPE);
		*out = node->literal;
		return true;
	case EXPR_VAR:
		if (node->a == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		*out = ctx->vars[node->a];
		return true;
	case EXPR_POINT:
		if (!device_get_point(ctx->dev, (space_kind_t)node->space, node->address, out))
			return fail(ctx, EXPR_FAIL_POINT);
		return true;
	case EXPR_NOW_MS:
		*out = (val_t){ .type = VAL_NUM, .n = (f64)ctx->now_ms };
		return true;
	case EXPR_ELAPSED_MS:
		*out = (val_t){ .type = VAL_NUM, .n = (f64)ctx->elapsed_ms };
		return true;
	case EXPR_ADD:
	case EXPR_SUB:
	case EXPR_MUL:
	case EXPR_DIV:
	case EXPR_MIN:
	case EXPR_MAX:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1) ||
		    !eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (lhs.type != VAL_NUM || rhs.type != VAL_NUM)
			return fail(ctx, EXPR_FAIL_TYPE);

		result = (val_t){ .type = VAL_NUM };
		switch (node->kind)
		{
		case EXPR_ADD: result.n = lhs.n + rhs.n; break;
		case EXPR_SUB: result.n = lhs.n - rhs.n; break;
		case EXPR_MUL: result.n = lhs.n * rhs.n; break;
		case EXPR_DIV:
			if (rhs.n == 0.0)
				return fail(ctx, EXPR_FAIL_DIV_ZERO);
			result.n = lhs.n / rhs.n;
			break;
		case EXPR_MIN: result.n = fmin(lhs.n, rhs.n); break;
		case EXPR_MAX: result.n = fmax(lhs.n, rhs.n); break;
		default: return fail(ctx, EXPR_FAIL_TYPE);
		}
		*out = result;
		return true;
	case EXPR_MOD:
	case EXPR_BIT_AND:
	case EXPR_BIT_OR:
	case EXPR_BIT_XOR:
	case EXPR_SHIFT:
	case EXPR_BIT_SET:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1) ||
		    !eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (lhs.type != VAL_NUM || rhs.type != VAL_NUM)
			return fail(ctx, EXPR_FAIL_TYPE);

		if (node->kind == EXPR_MOD)
		{
			f64 r;

			if (rhs.n == 0.0)
				return fail(ctx, EXPR_FAIL_DIV_ZERO);
			r = fmod(lhs.n, rhs.n);
			if (r != 0.0 && (r < 0.0) != (rhs.n < 0.0))
				r += rhs.n;
			*out = (val_t){ .type = VAL_NUM, .n = r };
			return true;
		}
		else
		{
			u64 x = 0;
			u64 y = 0;
			bool negative = false;

			if (!whole_bits(lhs.n, &x))
				return fail(ctx, EXPR_FAIL_RANGE);
			if (node->kind == EXPR_SHIFT || node->kind == EXPR_BIT_SET)
			{
				/* N: whole number, shift in -53..53, bit index in 0..52 */
				f64 n = rhs.n;

				negative = n < 0.0;
				if (!(fabs(n) <= 53.0) || n != floor(n))
					return fail(ctx, EXPR_FAIL_RANGE);
				y = (u64)fabs(n);
				if (node->kind == EXPR_BIT_SET && (negative || y > 52))
					return fail(ctx, EXPR_FAIL_RANGE);
			}
			else if (!whole_bits(rhs.n, &y))
				return fail(ctx, EXPR_FAIL_RANGE);

			switch (node->kind)
			{
			case EXPR_BIT_AND: x &= y; break;
			case EXPR_BIT_OR: x |= y; break;
			case EXPR_BIT_XOR: x ^= y; break;
			case EXPR_SHIFT:
				if (negative)
					x >>= y;
				else if (x != 0 && (x >> (53 - y)) != 0)
					return fail(ctx, EXPR_FAIL_RANGE);
				else
					x <<= y;
				break;
			case EXPR_BIT_SET:
				*out = (val_t){ .type = VAL_BOOL, .b = ((x >> y) & 1u) != 0 };
				return true;
			default: return fail(ctx, EXPR_FAIL_TYPE);
			}
			*out = (val_t){ .type = VAL_NUM, .n = (f64)x };
			return true;
		}
	case EXPR_ABS:
	case EXPR_FLOOR:
	case EXPR_ROUND:
	case EXPR_TRUNCATE:
		if (node->a == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1))
			return false;
		if (lhs.type != VAL_NUM)
			return fail(ctx, EXPR_FAIL_TYPE);

		result = (val_t){ .type = VAL_NUM };
		switch (node->kind)
		{
		case EXPR_ABS: result.n = fabs(lhs.n); break;
		case EXPR_FLOOR: result.n = floor(lhs.n); break;
		case EXPR_ROUND: result.n = nearbyint(lhs.n); break;	/* default mode: half to even */
		case EXPR_TRUNCATE: result.n = trunc(lhs.n); break;
		default: return fail(ctx, EXPR_FAIL_TYPE);
		}
		*out = result;
		return true;
	case EXPR_LT:
	case EXPR_LE:
	case EXPR_GT:
	case EXPR_GE:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1) ||
		    !eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (lhs.type != VAL_NUM || rhs.type != VAL_NUM)
			return fail(ctx, EXPR_FAIL_TYPE);

		result = (val_t){ .type = VAL_BOOL };
		switch (node->kind)
		{
		case EXPR_LT: result.b = lhs.n < rhs.n; break;
		case EXPR_LE: result.b = lhs.n <= rhs.n; break;
		case EXPR_GT: result.b = lhs.n > rhs.n; break;
		case EXPR_GE: result.b = lhs.n >= rhs.n; break;
		default: return fail(ctx, EXPR_FAIL_TYPE);
		}
		*out = result;
		return true;
	case EXPR_EQ:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1) ||
		    !eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (lhs.type != rhs.type)
			return fail(ctx, EXPR_FAIL_TYPE);

		*out = (val_t){ .type = VAL_BOOL };
		out->b = lhs.type == VAL_BOOL ? lhs.b == rhs.b : lhs.n == rhs.n;
		return true;
	case EXPR_AND:
	case EXPR_OR:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1))
			return false;
		if (lhs.type != VAL_BOOL)
			return fail(ctx, EXPR_FAIL_TYPE);
		if ((node->kind == EXPR_AND && !lhs.b) ||
		    (node->kind == EXPR_OR && lhs.b))
		{
			*out = (val_t){ .type = VAL_BOOL, .b = lhs.b };
			return true;
		}
		if (!eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (rhs.type != VAL_BOOL)
			return fail(ctx, EXPR_FAIL_TYPE);

		*out = (val_t){ .type = VAL_BOOL, .b = rhs.b };
		return true;
	case EXPR_NOT:
		if (node->a == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1))
			return false;
		if (lhs.type != VAL_BOOL)
			return fail(ctx, EXPR_FAIL_TYPE);

		*out = (val_t){ .type = VAL_BOOL, .b = !lhs.b };
		return true;
	case EXPR_IF:
		if (node->a == EXPR_NO_CHILD ||
		    node->b == EXPR_NO_CHILD ||
		    node->c == EXPR_NO_CHILD)
			return fail(ctx, EXPR_FAIL_TYPE);

		/* Branch type equality is a load-time check; only one branch runs. */
		if (!eval_node(nodes, node->a, ctx, &cond, depth + 1))
			return false;
		if (cond.type != VAL_BOOL)
			return fail(ctx, EXPR_FAIL_TYPE);

		chosen = cond.b ? node->b : node->c;
		return eval_node(nodes, chosen, ctx, out, depth + 1);
	default:
		return fail(ctx, EXPR_FAIL_TYPE);
	}
}

bool
expr_eval(const expr_node_t* nodes, u16 root, expr_ctx_t* ctx, val_t* out)
{
	ctx->failure = EXPR_FAIL_NONE;
	return eval_node(nodes, root, ctx, out, 0);
}
