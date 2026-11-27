/**
 * @file
 * @brief Compiled machine expression nodes and their evaluator.
 */
#ifndef EXPR_H
#define EXPR_H

#include "device/device.h"
#include "device/value.h"

typedef enum expr_kind_t expr_kind_t;
typedef struct expr_node_t expr_node_t;
typedef enum expr_failure_t expr_failure_t;
typedef struct expr_ctx_t expr_ctx_t;

/** @brief Operation of one expression node. */
enum expr_kind_t
{
	EXPR_BOOL, EXPR_NUM, EXPR_VAR, EXPR_POINT,
	EXPR_NOW_MS, EXPR_ELAPSED_MS,
	EXPR_ADD, EXPR_SUB, EXPR_MUL, EXPR_DIV, EXPR_MIN, EXPR_MAX,
	EXPR_LT, EXPR_LE, EXPR_GT, EXPR_GE, EXPR_EQ,
	EXPR_AND, EXPR_OR, EXPR_NOT, EXPR_IF,
	EXPR_MOD, EXPR_ABS, EXPR_FLOOR, EXPR_ROUND, EXPR_TRUNCATE,
	EXPR_BIT_AND, EXPR_BIT_OR, EXPR_BIT_XOR, EXPR_SHIFT, EXPR_BIT_SET
};

#define EXPR_NO_CHILD UINT16_MAX

/** @brief One node of a compiled expression tree; children are node indices. */
struct expr_node_t
{
	expr_kind_t	kind;
	u8		space;		/**< EXPR_POINT: numeric space_kind_t value (0..3) */
	u16		a, b, c;	/**< child indices / var index; unused = EXPR_NO_CHILD */
	u32		address;	/**< EXPR_POINT */
	val_t		literal;	/**< EXPR_BOOL / EXPR_NUM */
};

/** @brief Why an expression evaluation failed. */
enum expr_failure_t
{
	EXPR_FAIL_NONE,
	EXPR_FAIL_BUDGET,
	EXPR_FAIL_DIV_ZERO,
	EXPR_FAIL_POINT,
	EXPR_FAIL_TYPE,
	EXPR_FAIL_RANGE,	/**< bit operand not a whole number in 0..2^53-1, or shift/bit index out of range */
};

/** @brief Inputs and limits of one expression evaluation. */
struct expr_ctx_t
{
	device_t*	dev;
	val_t*		vars;		/**< the machine's variables */
	u64		now_ms;
	u64		elapsed_ms;
	u32		budget;		/**< steps left; one per node evaluated */
	expr_failure_t	failure;	/**< reason of the last failed eval, for logging */
};

bool expr_eval(const expr_node_t* nodes, u16 root, expr_ctx_t* ctx, val_t* out);

/** @brief Log text of an evaluation failure ("" for EXPR_FAIL_NONE or an unknown value). */
const char* expr_failure_text(expr_failure_t why);

#endif
