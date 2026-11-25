#include "device/expr.h"
#include <math.h>

/* The point accessor arrives with the device-machine task. Keeping this
 * reference weak lets T1 build on its own; the tests provide a stub. */
bool __attribute__((weak)) device_get_point(const device_t* dev, space_kind_t space,
					    u32 address, val_t* out);

enum { EXPR_MAX_DEPTH = 512 };

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
		return false;
	if (ctx->budget == 0)
		return false;
	ctx->budget--;

	switch (node->kind)
	{
	case EXPR_BOOL:
		if (node->literal.type != VAL_BOOL)
			return false;
		*out = node->literal;
		return true;
	case EXPR_NUM:
		if (node->literal.type != VAL_NUM)
			return false;
		*out = node->literal;
		return true;
	case EXPR_VAR:
		if (node->a == EXPR_NO_CHILD)
			return false;
		*out = ctx->vars[node->a];
		return true;
	case EXPR_POINT:
		if (!device_get_point)
			return false;
		return device_get_point(ctx->dev, (space_kind_t)node->space, node->address, out);
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
			return false;
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1) ||
		    !eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (lhs.type != VAL_NUM || rhs.type != VAL_NUM)
			return false;

		result = (val_t){ .type = VAL_NUM };
		switch (node->kind)
		{
		case EXPR_ADD: result.n = lhs.n + rhs.n; break;
		case EXPR_SUB: result.n = lhs.n - rhs.n; break;
		case EXPR_MUL: result.n = lhs.n * rhs.n; break;
		case EXPR_DIV:
			if (rhs.n == 0.0)
				return false;
			result.n = lhs.n / rhs.n;
			break;
		case EXPR_MIN: result.n = fmin(lhs.n, rhs.n); break;
		case EXPR_MAX: result.n = fmax(lhs.n, rhs.n); break;
		default: return false;
		}
		*out = result;
		return true;
	case EXPR_LT:
	case EXPR_LE:
	case EXPR_GT:
	case EXPR_GE:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return false;
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1) ||
		    !eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (lhs.type != VAL_NUM || rhs.type != VAL_NUM)
			return false;

		result = (val_t){ .type = VAL_BOOL };
		switch (node->kind)
		{
		case EXPR_LT: result.b = lhs.n < rhs.n; break;
		case EXPR_LE: result.b = lhs.n <= rhs.n; break;
		case EXPR_GT: result.b = lhs.n > rhs.n; break;
		case EXPR_GE: result.b = lhs.n >= rhs.n; break;
		default: return false;
		}
		*out = result;
		return true;
	case EXPR_EQ:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return false;
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1) ||
		    !eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (lhs.type != rhs.type)
			return false;

		*out = (val_t){ .type = VAL_BOOL };
		out->b = lhs.type == VAL_BOOL ? lhs.b == rhs.b : lhs.n == rhs.n;
		return true;
	case EXPR_AND:
	case EXPR_OR:
		if (node->a == EXPR_NO_CHILD || node->b == EXPR_NO_CHILD)
			return false;
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1))
			return false;
		if (lhs.type != VAL_BOOL)
			return false;
		if ((node->kind == EXPR_AND && !lhs.b) ||
		    (node->kind == EXPR_OR && lhs.b))
		{
			*out = (val_t){ .type = VAL_BOOL, .b = lhs.b };
			return true;
		}
		if (!eval_node(nodes, node->b, ctx, &rhs, depth + 1))
			return false;
		if (rhs.type != VAL_BOOL)
			return false;

		*out = (val_t){ .type = VAL_BOOL, .b = rhs.b };
		return true;
	case EXPR_NOT:
		if (node->a == EXPR_NO_CHILD)
			return false;
		if (!eval_node(nodes, node->a, ctx, &lhs, depth + 1))
			return false;
		if (lhs.type != VAL_BOOL)
			return false;

		*out = (val_t){ .type = VAL_BOOL, .b = !lhs.b };
		return true;
	case EXPR_IF:
		if (node->a == EXPR_NO_CHILD ||
		    node->b == EXPR_NO_CHILD ||
		    node->c == EXPR_NO_CHILD)
			return false;

		/* Branch type equality is a load-time check; only one branch runs. */
		if (!eval_node(nodes, node->a, ctx, &cond, depth + 1))
			return false;
		if (cond.type != VAL_BOOL)
			return false;

		chosen = cond.b ? node->b : node->c;
		return eval_node(nodes, chosen, ctx, out, depth + 1);
	default:
		return false;
	}
}

bool
expr_eval(const expr_node_t* nodes, u16 root, expr_ctx_t* ctx, val_t* out)
{
	return eval_node(nodes, root, ctx, out, 0);
}
