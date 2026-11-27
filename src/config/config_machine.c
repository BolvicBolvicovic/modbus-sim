/**
 * @file
 * @brief Compiles (machine ...) forms into the machine runtime tables.
 */
#include "config/config_machine.h"
#include "config/config_fault.h"
#include "vlib/macros.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct point_ref_t point_ref_t;
typedef struct machine_state_t machine_state_t;
typedef struct staged_stmt_t staged_stmt_t;
typedef struct top_list_t top_list_t;
typedef struct compile_t compile_t;
typedef enum operand_rule_t operand_rule_t;
typedef enum result_rule_t result_rule_t;
typedef struct expr_form_t expr_form_t;

/** @brief Kinds of statement handlers a machine state can have. */
enum
{
	HANDLER_ENTER,
	HANDLER_TICK,
	HANDLER_EXIT,
	HANDLER_WRITE,
	HANDLER_COUNT,
};

static const char* const HANDLER_NAMES[HANDLER_COUNT] = {
	"on-enter", "on-tick", "on-exit", "on-write",
};

#define MACHINE_POINT_REF_MAX \
	(MACHINE_MAX_MACHINES * (MACHINE_MAX_STATEMENTS + MACHINE_MAX_EXPR_NODES + \
				 MACHINE_MAX_WRITE_HANDLERS + 1))
#define EMITTED_NONE UINT16_MAX
#define NO_CHILD_SLOT UINT16_MAX

/** @brief A point referenced by a machine statement or expression, resolved after parsing. */
struct point_ref_t
{
	u16		machine_index;
	u16		state_index;
	u16		expr_index;	/**< EMITTED_NONE for set! targets */
	space_kind_t	space;
	u32		address;
	const char*	text;		/**< copy of the s7 form in the scratch arena */
};

/** @brief Parser state lives in the scratch arena, never in a device or machine_def_t. */
struct machine_state_t
{
	point_ref_t	point_refs[MACHINE_POINT_REF_MAX];
	u32		point_ref_count;
	u16		ref_by_stmt[MACHINE_MAX_MACHINES][MACHINE_MAX_STATEMENTS];
	u16		state_register_ref[MACHINE_MAX_MACHINES];
	u16		write_ref[MACHINE_MAX_MACHINES][MACHINE_MAX_WRITE_HANDLERS];
};

/** @brief A parsed statement waiting for its children before it is emitted in order. */
struct staged_stmt_t
{
	machine_stmt_t	stmt;
	s7_pointer	form;
	u16		first_child_staged;
	u16		next_sibling;
	u16		state_index;
	u8		handler;
	bool		top;
	u16		ref_index;	/**< point_refs index + 1, for set! on a point */
};

/** @brief Indices of the top-level statements of one handler. */
struct top_list_t
{
	u16	indices[MACHINE_MAX_STATEMENTS];
	u16	count;
};

/** @brief Working state while compiling the statements of one machine state. */
struct compile_t
{
	form_ctx_t*	ctx;
	machine_state_t* state;
	machine_def_t*	def;
	u16		machine_index;
	u16		state_index;
	u8		handler;
	const char*	machine_name;
	const char*	state_name;
	staged_stmt_t	staged[MACHINE_MAX_STATEMENTS];
	u32		staged_count;
	top_list_t	tops[MACHINE_MAX_STATES][HANDLER_COUNT];
	top_list_t	write_tops[MACHINE_MAX_WRITE_HANDLERS];
	u16		emitted[MACHINE_MAX_STATEMENTS];
};

#define EXPR_ARITY_ANY UINT8_MAX

/** @brief Which operand types an expression form accepts. */
enum operand_rule_t
{
	OPERAND_NONE,
	OPERAND_NUMBERS,
	OPERAND_SAME_TYPE,
	OPERAND_BOOLEANS,
};

/** @brief How the result type of an expression form is derived. */
enum result_rule_t
{
	RESULT_NUM,
	RESULT_BOOL,
	RESULT_BRANCHES,	/**< IF: type of the compiled branches */
};

/** @brief Compiles one expression form into expression nodes. */
typedef bool (*expr_compile_fn)(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);

/** @brief One expression form of the machine language: name, arity and typing rules. */
struct expr_form_t
{
	const char*	name;
	expr_kind_t	kind;
	expr_compile_fn	compile;
	operand_rule_t	operands;
	result_rule_t	result;
	space_kind_t	space;		/**< SPACE_COUNT for non-point forms */
	u8		min_len;
	u8		max_len;	/**< EXPR_ARITY_ANY for variadic forms */
};

static bool compile_point(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);
static bool compile_nullary(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);
static bool compile_binary(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);
static bool compile_logical(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);
static bool compile_unary(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);
static bool compile_not(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);
static bool compile_if(compile_t*, s7_pointer, const expr_form_t*, val_type_t*, u16*);

/**
 * @brief Lengths are s7 list lengths including the head; point forms validate their own
 *        address form.
 */
static const expr_form_t EXPR_FORMS[] = {
	{ "coil",      EXPR_POINT,      compile_point,   OPERAND_NONE, RESULT_BOOL, SPACE_COIL,     2, 2 },
	{ "discrete",  EXPR_POINT,      compile_point,   OPERAND_NONE, RESULT_BOOL, SPACE_DISCRETE, 2, 2 },
	{ "input",     EXPR_POINT,      compile_point,   OPERAND_NONE, RESULT_NUM,  SPACE_INPUT,    2, 2 },
	{ "holding",   EXPR_POINT,      compile_point,   OPERAND_NONE, RESULT_NUM,  SPACE_HOLDING,  2, 2 },
	{ "now-ms",    EXPR_NOW_MS,     compile_nullary, OPERAND_NONE, RESULT_NUM,  SPACE_COUNT,    1, 1 },
	{ "elapsed-ms", EXPR_ELAPSED_MS, compile_nullary, OPERAND_NONE, RESULT_NUM, SPACE_COUNT,    1, 1 },
	{ "+",         EXPR_ADD,        compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "-",         EXPR_SUB,        compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "*",         EXPR_MUL,        compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "/",         EXPR_DIV,        compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "min",       EXPR_MIN,        compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "max",       EXPR_MAX,        compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "mod",       EXPR_MOD,        compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "abs",       EXPR_ABS,        compile_unary,   OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 2, 2 },
	{ "floor",     EXPR_FLOOR,      compile_unary,   OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 2, 2 },
	{ "round",     EXPR_ROUND,      compile_unary,   OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 2, 2 },
	{ "truncate",  EXPR_TRUNCATE,   compile_unary,   OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 2, 2 },
	{ "bit-and",   EXPR_BIT_AND,    compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "bit-or",    EXPR_BIT_OR,     compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "bit-xor",   EXPR_BIT_XOR,    compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "shift",     EXPR_SHIFT,      compile_binary,  OPERAND_NUMBERS,   RESULT_NUM,  SPACE_COUNT, 3, 3 },
	{ "bit-set?",  EXPR_BIT_SET,    compile_binary,  OPERAND_NUMBERS,   RESULT_BOOL, SPACE_COUNT, 3, 3 },
	{ "<",         EXPR_LT,         compile_binary,  OPERAND_NUMBERS,   RESULT_BOOL, SPACE_COUNT, 3, 3 },
	{ "<=",        EXPR_LE,         compile_binary,  OPERAND_NUMBERS,   RESULT_BOOL, SPACE_COUNT, 3, 3 },
	{ ">",         EXPR_GT,         compile_binary,  OPERAND_NUMBERS,   RESULT_BOOL, SPACE_COUNT, 3, 3 },
	{ ">=",        EXPR_GE,         compile_binary,  OPERAND_NUMBERS,   RESULT_BOOL, SPACE_COUNT, 3, 3 },
	{ "=",         EXPR_EQ,         compile_binary,  OPERAND_SAME_TYPE, RESULT_BOOL, SPACE_COUNT, 3, 3 },
	{ "and",       EXPR_AND,        compile_logical, OPERAND_BOOLEANS, RESULT_BOOL, SPACE_COUNT, 3, EXPR_ARITY_ANY },
	{ "or",        EXPR_OR,         compile_logical, OPERAND_BOOLEANS, RESULT_BOOL, SPACE_COUNT, 3, EXPR_ARITY_ANY },
	{ "not",       EXPR_NOT,        compile_not,     OPERAND_BOOLEANS, RESULT_BOOL, SPACE_COUNT, 2, 2 },
	/* IF's operand rule applies to its condition; branches only need matching types. */
	{ "if",        EXPR_IF,         compile_if,      OPERAND_BOOLEANS, RESULT_BRANCHES, SPACE_COUNT, 4, 4 },
};

static const expr_form_t*
find_expr_form(const char* head)
{
	for (u32 i = 0; i < ARRAY_COUNT(EXPR_FORMS); i++)
	{
		if (strcmp(head, EXPR_FORMS[i].name) == 0)
			return &EXPR_FORMS[i];
	}
	return NULL;
}

static bool
name_ok(const char* name)
{
	if (!name ||
	    name[0] == '\0' ||
	    strlen(name) >= MACHINE_NAME_MAX)
		return false;
	if (name[0] < 'a' || name[0] > 'z')
		return false;
	for (const char* p = name; *p; p++)
	{
		if ((*p >= 'a' && *p <= 'z') ||
		    (*p >= '0' && *p <= '9') ||
		    *p == '-')
			continue;
		return false;
	}
	return true;
}

static const char*
copy_form_text(form_ctx_t* ctx, s7_pointer form)
{
	const char*	text = form_text(ctx, form);
	u32		len  = (u32)strlen(text) + 1;
	char*		copy = ARENA_PUSH_ARRAY(ctx->scratch, char, len);

	if (!copy)
	{
		form_fail(ctx, "out of memory while reading a machine form");
		return NULL;
	}
	memcpy(copy, text, len);
	return copy;
}

static bool
fail_at(compile_t* c, s7_pointer form, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));

static bool
fail_at(compile_t* c, s7_pointer form, const char* fmt, ...)
{
	char	detail[240];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	return form_fail(c->ctx, "machine '%s', state '%s', %s: %s",
			 c->machine_name ? c->machine_name : "(unknown)",
			 c->state_name ? c->state_name : "(none)",
			 form_text(c->ctx, form), detail);
}

static bool
fail_machine(compile_t* c, const char* form, const char* reason)
{
	return form_fail(c->ctx, "machine '%s', %s: %s",
			 c->machine_name ? c->machine_name : "(unknown)", form, reason);
}

static machine_state_t*
get_machine_state(form_ctx_t* ctx)
{
	machine_state_t* state = NULL;

	if (ctx->machine_state)
		return ctx->machine_state;
	if (!ctx->scratch)
	{
		form_fail(ctx, "config has no scratch arena for machine forms");
		return NULL;
	}
	state = ARENA_PUSH_ARRAY_ALIGNED(ctx->scratch, machine_state_t, 1, 8);
	if (!state)
	{
		form_fail(ctx, "out of memory while reading machine forms");
		return NULL;
	}
	ctx->machine_state = state;
	return state;
}

static bool
is_head(s7_pointer form, const char* head)
{
	const char* found = form_head_name(form);

	return found && strcmp(found, head) == 0;
}

static bool
parse_literal(form_ctx_t* ctx, s7_pointer form, val_t* out)
{
	if (s7_is_boolean(form))
	{
		*out = (val_t){ .type = VAL_BOOL, .b = s7_boolean(ctx->sc, form) };
		return true;
	}
	if (s7_is_integer(form) || s7_is_real(form))
	{
		f64 number = s7_is_integer(form) ? (f64)s7_integer(form) : (f64)s7_real(form);

		if (!isfinite(number))
			return false;
		*out = (val_t){ .type = VAL_NUM, .n = number };
		return true;
	}
	return false;
}

static u16
find_var(const machine_def_t* def, const char* name)
{
	for (u16 i = 0; i < def->var_count; i++)
	{
		if (strcmp(def->vars[i].name, name) == 0)
			return i;
	}
	return MACHINE_MAX_VARS;
}

static u16
find_state(const machine_def_t* def, const char* name)
{
	for (u16 i = 0; i < def->state_count; i++)
	{
		if (strcmp(def->states[i].name, name) == 0)
			return i;
	}
	return MACHINE_MAX_STATES;
}

static bool
add_node(compile_t* c, s7_pointer form, expr_node_t node, u16* out)
{
	if (c->def->node_count >= MACHINE_MAX_EXPR_NODES)
		return fail_at(c, form, "MACHINE_MAX_EXPR_NODES exceeded");
	*out = c->def->node_count++;
	c->def->nodes[*out] = node;
	return true;
}

static bool
add_point_ref(compile_t* c, s7_pointer form, space_kind_t space, u32 address,
	      u16 expr_index, u16* out)
{
	const char* text = NULL;

	if (c->state->point_ref_count >= MACHINE_POINT_REF_MAX)
		return fail_at(c, form, "too many machine point references");
	text = copy_form_text(c->ctx, form);
	if (!text)
		return false;
	*out = ++c->state->point_ref_count;
	c->state->point_refs[*out - 1] = (point_ref_t){
		.machine_index = c->machine_index,
		.state_index = c->state_index,
		.expr_index = expr_index,
		.space = space,
		.address = address,
		.text = text,
	};
	return true;
}

static bool
parse_point_form(compile_t* c, s7_pointer form, space_kind_t* space, u32* address)
{
	const char* head = form_head_name(form);
	const expr_form_t* spec = NULL;
	s7_pointer	addr = NULL;
	s64		value = 0;

	*space = SPACE_COUNT;
	if (!head)
		return fail_at(c, form, "expected a point like (holding 0)");
	spec = find_expr_form(head);
	if (!spec || spec->space == SPACE_COUNT)
		return fail_at(c, form, "unknown point space");
	*space = spec->space;
	if (s7_list_length(c->ctx->sc, form) != 2 ||
	    !s7_is_integer(s7_cadr(form)))
		return fail_at(c, form, "expected a whole-number point address");

	addr  = s7_cadr(form);
	value = (s64)s7_integer(addr);
	if (value < 0 || value > 65535)
		return fail_at(c, form, "point address must be between 0 and 65535");
	*address = (u32)value;
	return true;
}

static bool
statement_form(s7_pointer form)
{
	const char* head = form_head_name(form);

	return head && (strcmp(head, "set!") == 0 || strcmp(head, "when") == 0 ||
			strcmp(head, "unless") == 0);
}

static bool compile_expr(compile_t* c, s7_pointer form, val_type_t* type, u16* root);
static bool compile_stmt(compile_t* c, s7_pointer form, bool top);

static val_type_t
form_type(const expr_form_t* spec, val_type_t branch_type)
{
	if (spec->result == RESULT_BOOL)
		return VAL_BOOL;
	if (spec->result == RESULT_BRANCHES)
		return branch_type;
	return VAL_NUM;
}

static bool
boolean_operand_ok(const expr_form_t* spec, val_type_t type)
{
	return spec->operands != OPERAND_BOOLEANS || type == VAL_BOOL;
}

static bool
compile_point(compile_t* c, s7_pointer form, const expr_form_t* spec,
	      val_type_t* type, u16* root)
{
	space_kind_t	space = SPACE_COUNT;
	u32		address = 0;
	u16		ref = 0;
	expr_node_t	node = { 0 };

	if (!parse_point_form(c, form, &space, &address) ||
	    !add_point_ref(c, form, space, address, EMITTED_NONE, &ref))
		return false;
	node.kind = spec->kind;
	node.space = (u8)space;
	node.a = EXPR_NO_CHILD;
	node.b = EXPR_NO_CHILD;
	node.c = EXPR_NO_CHILD;
	node.address = address;
	*type = form_type(spec, VAL_NUM);
	if (!add_node(c, form, node, root))
		return false;
	c->state->point_refs[ref - 1].expr_index = *root;
	return true;
}

static bool
compile_nullary(compile_t* c, s7_pointer form, const expr_form_t* spec,
		val_type_t* type, u16* root)
{
	expr_node_t node = { 0 };

	if (s7_list_length(c->ctx->sc, form) != spec->max_len)
		return fail_at(c, form, "expected (%s) with no operands", spec->name);
	node.kind = spec->kind;
	node.a = EXPR_NO_CHILD;
	node.b = EXPR_NO_CHILD;
	node.c = EXPR_NO_CHILD;
	*type = form_type(spec, VAL_NUM);
	return add_node(c, form, node, root);
}

static bool
compile_binary(compile_t* c, s7_pointer form, const expr_form_t* spec,
	       val_type_t* type, u16* root)
{
	val_type_t	lhs = VAL_NUM;
	val_type_t	rhs = VAL_NUM;
	u16		a = 0;
	u16		b = 0;
	expr_node_t	node = { 0 };

	if (s7_list_length(c->ctx->sc, form) != spec->max_len)
		return fail_at(c, form, "expected exactly two operands");
	if (!compile_expr(c, s7_cadr(form), &lhs, &a) ||
	    !compile_expr(c, s7_caddr(form), &rhs, &b))
		return false;
	if (spec->operands == OPERAND_NUMBERS &&
	    (lhs != VAL_NUM || rhs != VAL_NUM))
		return fail_at(c, form, "operands must be numbers");
	if (spec->operands == OPERAND_SAME_TYPE && lhs != rhs)
		return fail_at(c, form, "equality operands must have the same type");
	node.kind = spec->kind;
	node.a = a;
	node.b = b;
	node.c = EXPR_NO_CHILD;
	*type = form_type(spec, VAL_NUM);
	return add_node(c, form, node, root);
}

static bool
compile_logical(compile_t* c, s7_pointer form, const expr_form_t* spec,
		val_type_t* type, u16* root)
{
	val_type_t second_type = VAL_NUM;
	u16	   next = 0;

	if (s7_list_length(c->ctx->sc, form) < spec->min_len)
		return fail_at(c, form, "expected at least two operands");
	if (!compile_expr(c, s7_cadr(form), type, root))
		return false;
	if (!boolean_operand_ok(spec, *type))
		return fail_at(c, s7_cadr(form), "logical operands must be booleans");
	for (s7_pointer p = s7_cddr(form); s7_is_pair(p); p = s7_cdr(p))
	{
		s7_pointer  operand = s7_car(p);
		expr_node_t node = { 0 };

		if (!compile_expr(c, operand, &second_type, &next))
			return false;
		if (!boolean_operand_ok(spec, second_type))
			return fail_at(c, operand, "logical operands must be booleans");
		node.kind = spec->kind;
		node.a = *root;
		node.b = next;
		node.c = EXPR_NO_CHILD;
		if (!add_node(c, form, node, root))
			return false;
	}
	*type = form_type(spec, VAL_NUM);
	return true;
}

static bool
compile_unary(compile_t* c, s7_pointer form, const expr_form_t* spec,
	      val_type_t* type, u16* root)
{
	val_type_t	first_type = VAL_NUM;
	u16		first = 0;
	expr_node_t	node = { 0 };

	if (s7_list_length(c->ctx->sc, form) != spec->max_len)
		return fail_at(c, form, "expected exactly one operand");
	if (!compile_expr(c, s7_cadr(form), &first_type, &first))
		return false;
	if (first_type != VAL_NUM)
		return fail_at(c, form, "operand must be a number");
	node.kind = spec->kind;
	node.a = first;
	node.b = EXPR_NO_CHILD;
	node.c = EXPR_NO_CHILD;
	*type = form_type(spec, VAL_NUM);
	return add_node(c, form, node, root);
}

static bool
compile_not(compile_t* c, s7_pointer form, const expr_form_t* spec,
	    val_type_t* type, u16* root)
{
	val_type_t	first_type = VAL_NUM;
	u16		first = 0;
	expr_node_t	node = { 0 };

	if (s7_list_length(c->ctx->sc, form) != spec->max_len)
		return fail_at(c, form, "expected exactly one operand");
	if (!compile_expr(c, s7_cadr(form), &first_type, &first))
		return false;
	if (!boolean_operand_ok(spec, first_type))
		return fail_at(c, form, "NOT operand must be a boolean");
	node.kind = spec->kind;
	node.a = first;
	node.b = EXPR_NO_CHILD;
	node.c = EXPR_NO_CHILD;
	*type = form_type(spec, VAL_NUM);
	return add_node(c, form, node, root);
}

static bool
compile_if(compile_t* c, s7_pointer form, const expr_form_t* spec,
	   val_type_t* type, u16* root)
{
	val_type_t	cond_type = VAL_NUM;
	val_type_t	then_type = VAL_NUM;
	val_type_t	else_type = VAL_NUM;
	u16		cond = 0;
	u16		then_root = 0;
	u16		else_root = 0;
	expr_node_t	node = { 0 };

	if (s7_list_length(c->ctx->sc, form) != spec->max_len)
		return fail_at(c, form, "expected (if CONDITION THEN ELSE)");
	if (!compile_expr(c, s7_cadr(form), &cond_type, &cond) ||
	    !compile_expr(c, s7_caddr(form), &then_type, &then_root) ||
	    !compile_expr(c, s7_caddr(s7_cdr(form)), &else_type, &else_root))
		return false;
	if (!boolean_operand_ok(spec, cond_type))
		return fail_at(c, s7_cadr(form), "IF condition must be a boolean");
	if (then_type != else_type)
		return fail_at(c, form, "IF branches must have the same type");
	node.kind = spec->kind;
	node.a = cond;
	node.b = then_root;
	node.c = else_root;
	*type = form_type(spec, then_type);
	return add_node(c, form, node, root);
}

static bool
compile_expr(compile_t* c, s7_pointer form, val_type_t* type, u16* root)
{
	val_t		literal = { 0 };
	expr_node_t	node = { 0 };
	const char*	head = NULL;
	const expr_form_t* spec = NULL;

	if (s7_is_boolean(form) ||
	    s7_is_integer(form) ||
	    s7_is_real(form))
	{
		if (!parse_literal(c->ctx, form, &literal))
			return fail_at(c, form, "number literal must be finite");
		node.kind = literal.type == VAL_BOOL ? EXPR_BOOL : EXPR_NUM;
		node.literal = literal;
		*type = literal.type;
		return add_node(c, form, node, root);
	}

	if (s7_is_symbol(form))
	{
		u16 index = find_var(c->def, s7_symbol_name(form));

		if (index == MACHINE_MAX_VARS)
			return fail_at(c, form, "unknown variable name");
		node.kind = EXPR_VAR;
		node.a = index;
		node.b = EXPR_NO_CHILD;
		node.c = EXPR_NO_CHILD;
		*type = c->def->vars[index].initial.type;
		return add_node(c, form, node, root);
	}

	if (!s7_is_pair(form))
		return fail_at(c, form, "unknown expression");
	head = form_head_name(form);
	if (!head)
		return fail_at(c, form, "unknown expression");
	if (statement_form(form))
		return fail_at(c, form, "statement form used as expression");
	spec = find_expr_form(head);
	if (!spec)
		return fail_at(c, form, "unknown expression form");
	return spec->compile(c, form, spec, type, root);
}

static bool
add_stmt(compile_t* c, s7_pointer form, machine_stmt_t stmt, bool top, u16 ref_index, u16* out)
{
	if (c->staged_count >= MACHINE_MAX_STATEMENTS)
		return fail_at(c, form, "MACHINE_MAX_STATEMENTS exceeded");
	*out = (u16)c->staged_count++;
	c->staged[*out] = (staged_stmt_t){
		.stmt = stmt,
		.form = form,
		.first_child_staged = NO_CHILD_SLOT,
		.next_sibling = NO_CHILD_SLOT,
		.state_index = c->state_index,
		.handler = c->handler,
		.top = top,
		.ref_index = ref_index,
	};
	return true;
}

static bool
compile_set(compile_t* c, s7_pointer form, bool top)
{
	val_type_t	value_type = VAL_NUM;
	val_type_t	target_type = VAL_NUM;
	machine_stmt_t	stmt = { .kind = STMT_SET };
	s7_pointer	target = s7_cadr(form);
	const char*	tname = NULL;
	u16		root = 0;
	u16		ref_index = 0;
	u16		index = 0;

	if (s7_list_length(c->ctx->sc, form) != 3)
		return fail_at(c, form, "expected (set! TARGET EXPR)");
	if (s7_is_symbol(target))
	{
		tname = s7_symbol_name(target);
		if (!name_ok(tname))
			return fail_at(c, target, "bad variable name");
		stmt.var_index = find_var(c->def, tname);
		if (stmt.var_index == MACHINE_MAX_VARS)
			return fail_at(c, target, "unknown variable name");
		stmt.target_kind = TARGET_VAR;
		target_type = c->def->vars[stmt.var_index].initial.type;
	}
	else if (s7_is_pair(target))
	{
		space_kind_t	space = SPACE_COUNT;
		u32		address = 0;

		if (!parse_point_form(c, target, &space, &address))
			return false;
		stmt.target_kind = TARGET_POINT;
		stmt.space = (u8)space;
		stmt.address = address;
		target_type = space_is_bit(space) ? VAL_BOOL : VAL_NUM;
		if (!add_point_ref(c, target, space, address, EMITTED_NONE, &ref_index))
			return false;
	}
	else
		return fail_at(c, target, "set! target must be a variable or a point");

	if (!compile_expr(c, s7_caddr(form), &value_type, &root))
		return false;
	if (value_type != target_type)
		return fail_at(c, form, "assignment value has the wrong type");
	stmt.expr_root = root;
	stmt.first_child = EXPR_NO_CHILD;
	return add_stmt(c, form, stmt, top, ref_index, &index);
}

static bool
compile_guard(compile_t* c, s7_pointer form, bool top)
{
	val_type_t	condition_type = VAL_NUM;
	machine_stmt_t	stmt = { 0 };
	u16		root = 0;
	u16		parent = 0;
	u16		previous = NO_CHILD_SLOT;

	if (s7_list_length(c->ctx->sc, form) < 3)
		return fail_at(c, form, "expected (%s CONDITION STATEMENT ...)",
			       form_head_name(form));
	if (!compile_expr(c, s7_cadr(form), &condition_type, &root))
		return false;
	if (condition_type != VAL_BOOL)
		return fail_at(c, s7_cadr(form), "condition must be a boolean");

	stmt.kind = is_head(form, "when") ? STMT_WHEN : STMT_UNLESS;
	stmt.expr_root = root;
	stmt.first_child = EXPR_NO_CHILD;
	if (!add_stmt(c, form, stmt, top, 0, &parent))
		return false;

	for (s7_pointer p = s7_cddr(form); s7_is_pair(p); p = s7_cdr(p))
	{
		u16 child = (u16)c->staged_count;

		if (!compile_stmt(c, s7_car(p), false))
			return false;
		if (previous == NO_CHILD_SLOT)
			c->staged[parent].first_child_staged = child;
		else
			c->staged[previous].next_sibling = child;
		previous = child;
	}
	return true;
}

static bool
compile_stmt(compile_t* c, s7_pointer form, bool top)
{
	if (!form_head_name(form))
		return fail_at(c, form, "expected a statement like (set! v 1)");
	if (is_head(form, "set!"))
		return compile_set(c, form, top);
	if (is_head(form, "when") || is_head(form, "unless"))
		return compile_guard(c, form, top);
	return fail_at(c, form, "expression form used as statement");
}

static void
set_handler_range(compile_t* c, u16 state_index, u8 handler, u16 first, u16 count)
{
	machine_state_def_t* state = &c->def->states[state_index];

	if (handler == HANDLER_ENTER)
	{
		state->on_enter_first = first;
		state->on_enter_count = count;
	}
	else if (handler == HANDLER_TICK)
	{
		state->on_tick_first = first;
		state->on_tick_count = count;
	}
	else if (handler == HANDLER_EXIT)
	{
		state->on_exit_first = first;
		state->on_exit_count = count;
	}
}

static bool
emit_stmt(compile_t* c, u32 staged_index)
{
	staged_stmt_t* staged = &c->staged[staged_index];
	u16 index = 0;

	if (c->emitted[staged_index] != EMITTED_NONE)
		return true;
	if (c->def->statement_count >= MACHINE_MAX_STATEMENTS)
		return fail_at(c, staged->form, "MACHINE_MAX_STATEMENTS exceeded");
	index = c->def->statement_count++;
	c->def->statements[index] = staged->stmt;
	c->emitted[staged_index] = index;
	if (staged->ref_index)
		c->state->ref_by_stmt[c->machine_index][index] = staged->ref_index;
	return true;
}

static bool
flatten_statements(compile_t* c)
{
	memset(c->emitted, 0xFF, sizeof(c->emitted));

	for (u16 state = 0; state < c->def->state_count; state++)
	{
		for (u8 handler = 0; handler < HANDLER_WRITE; handler++)
		{
			top_list_t* list = &c->tops[state][handler];

			if (list->count == 0)
				continue;
			for (u16 i = 0; i < list->count; i++)
			{
				if (!emit_stmt(c, list->indices[i]))
					return false;
			}
			set_handler_range(c, state, handler, c->emitted[list->indices[0]],
					  list->count);
		}
	}

	for (u16 h = 0; h < c->def->write_handler_count; h++)
	{
		top_list_t* list = &c->write_tops[h];

		if (list->count == 0)
			return fail_machine(c, "on-write", "on-write requires at least one statement");
		for (u16 i = 0; i < list->count; i++)
		{
			if (!emit_stmt(c, list->indices[i]))
				return false;
		}
		c->def->write_handlers[h].first = c->emitted[list->indices[0]];
		c->def->write_handlers[h].count = list->count;
	}

	for (u32 i = 0; i < c->staged_count; i++)
	{
		staged_stmt_t* staged = &c->staged[i];
		u16 children[MACHINE_MAX_STATEMENTS];
		u16 parent = 0;
		u16 first = 0;
		u32 count = 0;

		if (staged->first_child_staged == NO_CHILD_SLOT ||
		    c->emitted[i] == EMITTED_NONE)
			continue;
		for (u16 child = staged->first_child_staged; child != NO_CHILD_SLOT;
		     child = c->staged[child].next_sibling)
		{
			if (count >= MACHINE_MAX_STATEMENTS)
				return fail_at(c, staged->form, "MACHINE_MAX_STATEMENTS exceeded");
			children[count++] = child;
		}
		parent = c->emitted[i];
		first = c->def->statement_count;
		for (u32 j = 0; j < count; j++)
		{
			if (!emit_stmt(c, children[j]))
				return false;
		}
		c->def->statements[parent].first_child = first;
		c->def->statements[parent].child_count = (u16)count;
	}
	return true;
}

static bool
parse_var(compile_t* c, s7_pointer form)
{
	machine_var_def_t* var = NULL;
	val_t		initial = { 0 };
	const char*	name = NULL;

	if (s7_list_length(c->ctx->sc, form) != 3 || !s7_is_symbol(s7_cadr(form)))
		return fail_at(c, form, "expected (var NAME LITERAL)");
	name = s7_symbol_name(s7_cadr(form));
	if (!name_ok(name))
		return fail_at(c, s7_cadr(form), "bad variable name");
	if (find_var(c->def, name) != MACHINE_MAX_VARS)
		return fail_at(c, form, "duplicate variable name");
	if (c->def->var_count >= MACHINE_MAX_VARS)
		return fail_at(c, form, "MACHINE_MAX_VARS exceeded");
	if (!parse_literal(c->ctx, s7_caddr(form), &initial))
		return fail_at(c, form, "variable literal must be a finite number, #t or #f");
	var = &c->def->vars[c->def->var_count++];
	strcpy(var->name, name);
	var->initial = initial;
	return true;
}

static bool
parse_handler(compile_t* c, s7_pointer form)
{
	top_list_t* list = &c->tops[c->state_index][c->handler];
	s7_pointer  p    = s7_cdr(form);

	for (; s7_is_pair(p); p = s7_cdr(p))
	{
		u16 top_index = (u16)c->staged_count;

		if (!compile_stmt(c, s7_car(p), true))
			return false;
		if (list->count >= MACHINE_MAX_STATEMENTS)
			return fail_at(c, s7_car(p), "MACHINE_MAX_STATEMENTS exceeded");
		list->indices[list->count++] = top_index;
	}
	if (!s7_is_null(c->ctx->sc, p))
		return fail_at(c, form, "malformed handler form");
	return true;
}

static bool
parse_state(compile_t* c, s7_pointer form)
{
	machine_state_def_t* state = NULL;
	const char*	name = NULL;
	const char*	previous = NULL;
	char		owner[MACHINE_NAME_MAX * 2 + 32];
	bool		seen[HANDLER_COUNT] = { false };
	s7_pointer	tail = NULL;

	if (s7_list_length(c->ctx->sc, form) < 2 || !s7_is_symbol(s7_cadr(form)))
		return fail_at(c, form, "expected (state NAME HANDLER ...)");
	name = s7_symbol_name(s7_cadr(form));
	if (!name_ok(name))
		return fail_at(c, s7_cadr(form), "bad state name");
	previous = c->state_name;
	c->state_name = name;
	snprintf(owner, sizeof(owner), "machine '%s', state '%s'",
		 c->machine_name ? c->machine_name : "(unknown)", name);
	for (u16 i = 0; i < c->def->state_count; i++)
	{
		if (strcmp(c->def->states[i].name, name) == 0)
			return fail_at(c, form, "duplicate state name");
	}
	if (c->def->state_count >= MACHINE_MAX_STATES)
		return fail_at(c, form, "MACHINE_MAX_STATES exceeded");

	state = &c->def->states[c->def->state_count++];
	strcpy(state->name, name);
	c->state_index = (u16)(c->def->state_count - 1);
	c->state_name = state->name;

	tail = s7_cddr(form);
	for (s7_pointer p = tail; s7_is_pair(p); p = s7_cdr(p))
	{
		s7_pointer handler_form = s7_car(p);
		bool	    found = false;

		if (is_head(handler_form, "fault"))
		{
			if (!config_parse_fault(c->ctx, handler_form, &state->faults, owner))
				return false;
			continue;
		}
		for (u8 h = 0; h < HANDLER_WRITE; h++)
		{
			if (!is_head(handler_form, HANDLER_NAMES[h]))
				continue;
			if (seen[h])
				return fail_at(c, handler_form, "handler '%s' is given more than once",
					       HANDLER_NAMES[h]);
			seen[h] = true;
			c->handler = h;
			found = true;
			break;
		}
		if (!found)
			return fail_at(c, handler_form, "unknown state handler");
		if (!parse_handler(c, handler_form))
			return false;
	}
	if (!s7_is_proper_list(c->ctx->sc, form))
		return fail_at(c, form, "malformed state form");

	c->state_name = previous;
	return true;
}

static bool
parse_after_ms(compile_t* c, s7_pointer form, machine_transition_def_t* transition)
{
	s7_pointer value = s7_cadr(form);
	s64	number = 0;

	if (is_head(value, "random"))
	{
		s7_pointer min_form = NULL;
		s7_pointer max_form = NULL;
		s64	min_ms = 0;
		s64	max_ms = 0;

		if (s7_list_length(c->ctx->sc, value) != 3)
			return fail_at(c, form, "expected (after-ms (random MIN MAX))");
		min_form = s7_cadr(value);
		max_form = s7_caddr(value);
		if (!s7_is_integer(min_form) || !s7_is_integer(max_form))
			return fail_at(c, form, "random bounds must be whole numbers");
		min_ms = (s64)s7_integer(min_form);
		max_ms = (s64)s7_integer(max_form);
		if (min_ms < 1 || max_ms > 86400000)
			return fail_at(c, form, "random after-ms bounds must be between 1 and 86400000");
		if (min_ms > max_ms)
			return fail_at(c, form, "random MIN must not be greater than MAX");
		transition->after_kind = AFTER_RANDOM;
		transition->after_min_ms = (u64)min_ms;
		transition->after_max_ms = (u64)max_ms;
		return true;
	}

	if (!s7_is_integer(value))
		return fail_at(c, form, "after-ms must be a whole number or (random MIN MAX)");
	number = (s64)s7_integer(value);
	if (number < 1 || number > 86400000)
		return fail_at(c, form, "after-ms must be between 1 and 86400000");
	transition->after_kind = AFTER_LITERAL;
	transition->after_literal_ms = (u64)number;
	return true;
}

static bool
parse_transition(compile_t* c, s7_pointer form)
{
	machine_transition_def_t* transition = NULL;
	const char*	from_name = NULL;
	const char*	to_name = NULL;
	const char*	previous_state = NULL;
	u16		from = 0;
	u16		to = 0;
	bool		has_when = false;
	bool		has_after = false;
	val_type_t	condition_type = VAL_NUM;

	previous_state = c->state_name;
	if (s7_is_pair(form) &&
	    s7_is_pair(s7_cdr(form)) &&
	    s7_is_symbol(s7_cadr(form)))
		c->state_name = s7_symbol_name(s7_cadr(form));
	if (c->def->transition_count >= MACHINE_MAX_TRANSITIONS)
		return fail_at(c, form, "MACHINE_MAX_TRANSITIONS exceeded");
	if (s7_list_length(c->ctx->sc, form) < 3 ||
	    !s7_is_symbol(s7_cadr(form)) ||
	    !s7_is_symbol(s7_caddr(form)))
		return fail_at(c, form, "expected (transition FROM TO TRIGGER ...)");
	from_name = s7_symbol_name(s7_cadr(form));
	to_name = s7_symbol_name(s7_caddr(form));
	if (!name_ok(from_name) || !name_ok(to_name))
		return fail_at(c, form, "bad state name in transition");
	from = find_state(c->def, from_name);
	to = find_state(c->def, to_name);
	c->state_name = from_name;
	c->state_index = from < MACHINE_MAX_STATES ? from : 0;
	if (from == MACHINE_MAX_STATES || to == MACHINE_MAX_STATES)
		return fail_at(c, form, "unknown initial or transition state");
	transition = &c->def->transitions[c->def->transition_count];

	s7_pointer triggers = s7_cddr(s7_cdr(form));

	for (s7_pointer p = triggers; s7_is_pair(p); p = s7_cdr(p))
	{
		s7_pointer	trigger = s7_car(p);
		const char*	head = form_head_name(trigger);

		if (!head || (strcmp(head, "when") != 0 && strcmp(head, "after-ms") != 0))
			return fail_at(c, trigger, "unknown transition trigger");
		if (strcmp(head, "when") == 0)
		{
			if (has_when)
				return fail_at(c, trigger, "duplicate (when ...) trigger");
			if (s7_list_length(c->ctx->sc, trigger) != 2)
				return fail_at(c, trigger, "expected (when EXPR)");
			if (!compile_expr(c, s7_cadr(trigger), &condition_type,
					  &transition->when_root))
				return false;
			if (condition_type != VAL_BOOL)
				return fail_at(c, trigger, "transition condition must be a boolean");
			has_when = true;
		}
		else
		{
			if (has_after)
				return fail_at(c, trigger, "duplicate (after-ms ...) trigger");
			if (s7_list_length(c->ctx->sc, trigger) != 2)
				return fail_at(c, trigger,
					       "expected (after-ms MS) or (after-ms (random MIN MAX))");
			if (!parse_after_ms(c, trigger, transition))
				return false;
			has_after = true;
		}
	}
	if (!has_when && !has_after)
		return fail_at(c, form, "transition has no trigger");
	transition->from = from;
	transition->to = to;
	transition->has_when = has_when;
	if (!has_after)
		transition->after_kind = AFTER_NONE;
	c->def->transition_count++;
	c->state_name = previous_state;
	return true;
}

static bool
parse_state_register(compile_t* c, s7_pointer form)
{
	space_kind_t	space = SPACE_COUNT;
	u32		address = 0;
	u16		ref = 0;

	if (s7_list_length(c->ctx->sc, form) != 2)
		return fail_at(c, form, "expected (state-register (holding N)) or (state-register (input N))");
	if (c->def->has_state_register)
		return fail_at(c, form, "state register is given more than once");
	if (!parse_point_form(c, s7_cadr(form), &space, &address))
		return false;
	if (space != SPACE_HOLDING && space != SPACE_INPUT)
		return fail_at(c, form, "state register must name a holding or input point");
	if (!add_point_ref(c, s7_cadr(form), space, address, EMITTED_NONE, &ref))
		return false;

	c->def->has_state_register = true;
	c->def->state_register_space = (u8)space;
	c->def->state_register_address = address;
	c->state->state_register_ref[c->machine_index] = ref;
	return true;
}

static bool
parse_on_write(compile_t* c, s7_pointer form)
{
	space_kind_t	space = SPACE_COUNT;
	u32		address = 0;
	u16		ref = 0;
	u16		handler = 0;
	top_list_t*	list = NULL;
	const char*	previous_state = c->state_name;

	c->state_name = NULL;
	/* On-write is machine-level: keep its refs out of state-name lookups. */
	c->state_index = MACHINE_MAX_STATES;
	c->handler = HANDLER_WRITE;
	if (s7_list_length(c->ctx->sc, form) < 3)
		return fail_at(c, form, "expected (on-write POINT STATEMENT ...)");
	if (!parse_point_form(c, s7_cadr(form), &space, &address))
		return false;
	for (u16 i = 0; i < c->def->write_handler_count; i++)
	{
		const machine_write_handler_t* other = &c->def->write_handlers[i];

		if (other->space == (u8)space && other->address == address)
			return fail_at(c, s7_cadr(form), "on-write is given more than once for the same point");
	}
	if (c->def->write_handler_count >= MACHINE_MAX_WRITE_HANDLERS)
		return fail_at(c, form, "MACHINE_MAX_WRITE_HANDLERS exceeded");
	if (!add_point_ref(c, s7_cadr(form), space, address, EMITTED_NONE, &ref))
		return false;

	handler = c->def->write_handler_count++;
	c->def->write_handlers[handler] = (machine_write_handler_t){
		.space = (u8)space,
		.address = address,
	};
	c->state->write_ref[c->machine_index][handler] = ref;
	list = &c->write_tops[handler];
	for (s7_pointer p = s7_cddr(form); s7_is_pair(p); p = s7_cdr(p))
	{
		u16 top_index = (u16)c->staged_count;

		if (!compile_stmt(c, s7_car(p), true))
			return false;
		if (list->count >= MACHINE_MAX_STATEMENTS)
			return fail_at(c, s7_car(p), "MACHINE_MAX_STATEMENTS exceeded");
		list->indices[list->count++] = top_index;
	}
	if (list->count == 0)
		return fail_at(c, form, "on-write requires at least one statement");
	if (!s7_is_proper_list(c->ctx->sc, form))
		return fail_at(c, form, "malformed on-write form");

	c->state_name = previous_state;
	return true;
}

static bool
parse_machine(compile_t* c, s7_pointer form, const char* machine_text, device_t* dev)
{
	const char*	initial_name = NULL;
	const char*	previous_state = NULL;
	long		len = s7_list_length(c->ctx->sc, form);
	enum { PHASE_VAR, PHASE_STATE, PHASE_TRANSITION } phase = PHASE_VAR;
	bool		seen_item = false;
	bool		seen_on_write = false;
	s7_pointer	tail = NULL;
	s7_pointer	p    = NULL;
	const char*	error = NULL;

	if (len < 3 || !s7_is_symbol(s7_cadr(form)))
		return form_fail(c->ctx, "%s: expected (machine NAME (initial STATE) ...)", machine_text);
	{
		const char* name = s7_symbol_name(s7_cadr(form));

		if (!name_ok(name))
			return form_fail(c->ctx, "%s: bad machine name", machine_text);
		for (u16 i = 0; i < dev->machines->count; i++)
		{
			if (strcmp(dev->machines->defs[i].name, name) == 0)
				return form_fail(c->ctx, "%s: machine '%s' is defined more than once",
						 machine_text, name);
		}
		strcpy(c->def->name, name);
	}
	c->machine_name = c->def->name;
	c->machine_index = dev->machines->count;

	if (!is_head(s7_caddr(form), "initial") ||
	    s7_list_length(c->ctx->sc, s7_caddr(form)) != 2 ||
	    !s7_is_symbol(s7_cadr(s7_caddr(form))))
		return fail_machine(c, machine_text, "expected (initial STATE)");
	initial_name = s7_symbol_name(s7_cadr(s7_caddr(form)));
	if (!name_ok(initial_name))
		return fail_machine(c, machine_text, "bad initial state name");

	tail = s7_cddr(s7_cdr(form));
	for (p = tail; s7_is_pair(p); p = s7_cdr(p))
	{
		s7_pointer	item = s7_car(p);
		const char*	head = form_head_name(item);

		if (!head)
			return fail_machine(c, machine_text,
					    "expected (var ...), (state-register ...), (state ...), "
					    "(on-write ...) or (transition ...)");
		if (strcmp(head, "state-register") == 0)
		{
			if (seen_item || c->def->has_state_register)
				return fail_machine(c, form_text(c->ctx, item),
						    "state-register must immediately follow (initial STATE)");
			seen_item = true;
			if (!parse_state_register(c, item))
				return false;
			continue;
		}
		seen_item = true;
		if (strcmp(head, "var") == 0)
		{
			if (phase != PHASE_VAR)
				return fail_machine(c, form_text(c->ctx, item),
						    "var must precede states and transitions");
			if (!parse_var(c, item))
				return false;
			continue;
		}
		if (strcmp(head, "state") == 0)
		{
			if (phase == PHASE_TRANSITION)
				return fail_machine(c, form_text(c->ctx, item),
						    "state must precede transitions");
			if (seen_on_write)
				return fail_machine(c, form_text(c->ctx, item),
						    "state must precede on-write handlers");
			phase = PHASE_STATE;
			if (!parse_state(c, item))
				return false;
			continue;
		}
		if (strcmp(head, "on-write") == 0)
		{
			if (phase != PHASE_STATE)
				return fail_machine(c, form_text(c->ctx, item),
						    "on-write must follow states and precede transitions");
			seen_on_write = true;
			if (!parse_on_write(c, item))
				return false;
			continue;
		}
		if (strcmp(head, "transition") == 0)
		{
			phase = PHASE_TRANSITION;
			if (!parse_transition(c, item))
				return false;
			continue;
		}
		return fail_machine(c, form_text(c->ctx, item), "unknown machine form");
	}
	if (!s7_is_null(c->ctx->sc, p))
		return fail_machine(c, machine_text, "malformed machine form");
	if (c->def->state_count == 0)
		return fail_machine(c, machine_text, "machine has no states");

	previous_state = c->state_name;
	c->state_name = initial_name;
	c->def->initial_state = find_state(c->def, initial_name);
	if (c->def->initial_state == MACHINE_MAX_STATES)
		error = "unknown initial state";
	c->state_name = previous_state;
	if (error)
		return fail_at(c, s7_cadr(s7_caddr(form)), "%s", error);

	if (!flatten_statements(c))
		return false;
	error = device_add_machine(dev, c->def);
	if (error)
		return form_fail(c->ctx, "%s: %s", machine_text, error);
	return true;
}

bool
config_machine_add(form_ctx_t* ctx, s7_pointer form, device_t* dev)
{
	compile_t		c = { 0 };
	machine_def_t		def = { 0 };
	machine_state_t*	state = NULL;
	const char*		text = NULL;
	const char*		error = NULL;

	error = device_machines_prepare(dev);
	if (error)
		return form_fail(ctx, "%s", error);
	if (dev->machines->count >= MACHINE_MAX_MACHINES)
		return form_fail(ctx, "%s: too many machines on one device (max %d)",
				 form_text(ctx, form), MACHINE_MAX_MACHINES);
	state = get_machine_state(ctx);
	if (!state)
		return false;
	text = copy_form_text(ctx, form);
	if (!text)
		return false;
	c.ctx = ctx;
	c.state = state;
	c.def = &def;
	return parse_machine(&c, form, text, dev);
}

static const char*
ref_state_name(const machine_def_t* def, const point_ref_t* ref)
{
	return ref && ref->state_index < def->state_count ?
	       def->states[ref->state_index].name : "(none)";
}

static const point_ref_t*
find_expr_ref(const machine_state_t* state, u16 machine_index, u16 expr_index)
{
	for (u32 i = 0; i < state->point_ref_count; i++)
	{
		const point_ref_t* ref = &state->point_refs[i];

		if (ref->machine_index == machine_index && ref->expr_index == expr_index)
			return ref;
	}
	return NULL;
}

static bool
check_expr_points(form_ctx_t* ctx, const machine_state_t* state, const device_t* dev,
		  u16 machine_index, const machine_def_t* def)
{
	for (u16 i = 0; i < def->node_count; i++)
	{
		const expr_node_t* node = &def->nodes[i];
		const point_ref_t* ref = NULL;
		const char* text = NULL;
		const char* state_name = NULL;
		u32 index = 0;

		if (node->kind != EXPR_POINT)
			continue;
		index = device_find_point(dev, (space_kind_t)node->space, node->address);
		if (index != DEVICE_MAX_POINTS &&
		    dev->points[index].type != VT_STRING)
			continue;
		ref = find_expr_ref(state, machine_index, i);
		text = ref ? ref->text : def->name;
		state_name = ref_state_name(def, ref);
		if (index != DEVICE_MAX_POINTS)
			return form_fail(ctx, "%s: machine '%s', state '%s': a string point has no number to read",
					 text, def->name, state_name);
		return form_fail(ctx, "%s: machine '%s', state '%s': point read must name the first address of a configured point",
				 text, def->name, state_name);
	}
	return true;
}

static bool
check_set_points(form_ctx_t* ctx, const machine_state_t* state, const device_t* dev,
		 u16 machine_index, const machine_def_t* def, u8* owners)
{
	for (u16 i = 0; i < def->statement_count; i++)
	{
		const machine_stmt_t* stmt = &def->statements[i];
		machine_target_result_t target = { 0 };
		u16 ref_index = 0;
		const point_ref_t* ref = NULL;
		const char* text = NULL;
		const char* state_name = NULL;

		if (stmt->kind != STMT_SET || stmt->target_kind != TARGET_POINT)
			continue;
		target = machine_check_target_point(dev, (space_kind_t)stmt->space,
						   stmt->address, owners,
						   (u8)(machine_index + 1));
		ref_index = state->ref_by_stmt[machine_index][i];
		if (ref_index)
			ref = &state->point_refs[ref_index - 1];
		text = ref ? ref->text : def->name;
		state_name = ref_state_name(def, ref);
		if (target.status == MACHINE_TARGET_NOT_FOUND)
			return form_fail(ctx, "%s: machine '%s', state '%s': point %s %u is not the first address of a configured point",
					 text, def->name, state_name,
					 space_name((space_kind_t)stmt->space), stmt->address);
		if (target.status == MACHINE_TARGET_STRING)
			return form_fail(ctx, "%s: machine '%s', state '%s': machine cannot drive a string point",
					 text, def->name, state_name);
		if (target.status == MACHINE_TARGET_RAMP)
			return form_fail(ctx, "%s: machine '%s', state '%s': machine cannot drive a ramp or modeled point",
					 text, def->name, state_name);
		if (target.status == MACHINE_TARGET_CONFLICT)
			return form_fail(ctx, "%s: machine '%s', state '%s': point %s %u is already driven by another machine",
					 text, def->name, state_name,
					 space_name((space_kind_t)stmt->space), stmt->address);
	}
	return true;
}

static const char*
ref_text(const machine_state_t* state, u16 ref_index)
{
	return ref_index ? state->point_refs[ref_index - 1].text : NULL;
}

static bool
check_state_registers(form_ctx_t* ctx, const machine_state_t* state, const device_t* dev,
		      const u8* set_owners, u8* reg_owners)
{
	for (u16 m = 0; m < dev->machines->count; m++)
	{
		const machine_def_t* def = &dev->machines->defs[m];
		const point_ref_t* ref = NULL;
		const char* text = NULL;
		u32 index = 0;

		if (!def->has_state_register)
			continue;
		ref = state->state_register_ref[m] ?
		      &state->point_refs[state->state_register_ref[m] - 1] : NULL;
		text = ref ? ref->text : "(state-register)";
		index = device_find_point(dev, (space_kind_t)def->state_register_space,
					  def->state_register_address);
		if (index == DEVICE_MAX_POINTS)
			return form_fail(ctx, "machine '%s': %s: state register must name the first address "
					      "of a configured point", def->name, text);
		if (dev->points[index].type == VT_STRING)
			return form_fail(ctx, "machine '%s': %s: state register cannot name a string point",
					 def->name, text);
		if (point_is_timed(&dev->points[index]))
			return form_fail(ctx, "machine '%s': %s: state register cannot name a ramp or modeled point",
					 def->name, text);
		if (set_owners[index] != 0)
			return form_fail(ctx, "machine '%s': %s: state register cannot name a point driven "
					      "by a machine", def->name, text);
		if (reg_owners[index] != 0)
			return form_fail(ctx, "machine '%s': %s: state register point is already used by "
					      "another machine", def->name, text);
		reg_owners[index] = (u8)(m + 1);
	}
	return true;
}

static bool
check_write_targets(form_ctx_t* ctx, const machine_state_t* state, const device_t* dev,
		    const u8* set_owners, const u8* reg_owners)
{
	for (u16 m = 0; m < dev->machines->count; m++)
	{
		const machine_def_t* def = &dev->machines->defs[m];

		for (u16 w = 0; w < def->write_handler_count; w++)
		{
			const machine_write_handler_t* h = &def->write_handlers[w];
			const char* text = ref_text(state, state->write_ref[m][w]);
			u32 index = device_find_point(dev, (space_kind_t)h->space, h->address);

			if (!text)
				text = "(on-write)";
			if (index == DEVICE_MAX_POINTS)
				return form_fail(ctx, "machine '%s': %s: on-write target must name the first "
						      "address of a configured point", def->name, text);
			if (dev->points[index].type == VT_STRING)
				return form_fail(ctx, "machine '%s': %s: on-write cannot watch a string point",
						 def->name, text);
			if (set_owners[index] != 0 ||
			    reg_owners[index] != 0 ||
			    dev->points[index].kind == POINT_MACHINE)
				return form_fail(ctx, "machine '%s': %s: on-write cannot watch a "
						      "machine-driven point", def->name, text);
		}
	}
	return true;
}

static bool
check_points(form_ctx_t* ctx, const machine_state_t* state, const device_t* dev)
{
	u8 set_owners[DEVICE_MAX_POINTS] = { 0 };
	u8 reg_owners[DEVICE_MAX_POINTS] = { 0 };

	for (u16 m = 0; m < dev->machines->count; m++)
	{
		const machine_def_t* def = &dev->machines->defs[m];

		if (!check_expr_points(ctx, state, dev, m, def) ||
		    !check_set_points(ctx, state, dev, m, def, set_owners))
			return false;
	}
	if (!check_state_registers(ctx, state, dev, set_owners, reg_owners))
		return false;
	return check_write_targets(ctx, state, dev, set_owners, reg_owners);
}

bool
config_machine_check_points(form_ctx_t* ctx, device_t* dev)
{
	if (!dev || !dev->machines)
		return true;
	if (ctx->machine_state && !check_points(ctx, ctx->machine_state, dev))
		return false;
	/* The same scratch state is reused for the next device. */
	if (ctx->machine_state)
	{
		machine_state_t* state = ctx->machine_state;

		state->point_ref_count = 0;
		memset(state->ref_by_stmt, 0, sizeof(state->ref_by_stmt));
		memset(state->state_register_ref, 0, sizeof(state->state_register_ref));
		memset(state->write_ref, 0, sizeof(state->write_ref));
	}
	return true;
}

bool
config_machine_finalize(form_ctx_t* ctx, device_t* dev)
{
	const char* error = NULL;

	if (!dev || !dev->machines)
		return true;
	error = device_finalize_machines(dev);
	if (error)
		return form_fail(ctx, "machine definitions: %s", error);
	return true;
}
