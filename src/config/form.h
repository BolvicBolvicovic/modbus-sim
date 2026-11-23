#ifndef FORM_H
#define FORM_H

/* Helpers to read the config forms that s7 hands back.
 *
 * A config is plain data: lists like (port 1502) or (holding 0 (static 5)).
 * "options" below means the list of such (key value...) forms inside a parent form.
 * Every function reports problems in ctx->err in plain words and returns false.
 * The opt_* functions leave `out` untouched when the option is absent. */

#include "vlib/c_types.h"
#include "third_party/s7/s7.h"
#include <stdbool.h>

typedef struct form_ctx_t form_ctx_t;
struct form_ctx_t
{
	s7_scheme*	sc;
	char*		err;
	u32		err_size;
};

/* Writes a printf-style message in ctx->err. Always returns false. */
bool		form_fail(form_ctx_t* ctx, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/* Prints a form the way it was written, for error messages. Valid until the next s7 allocation. */
const char*	form_text(form_ctx_t* ctx, s7_pointer form);

/* Name of the first symbol of (name ...). NULL if form is not such a list. */
const char*	form_head_name(s7_pointer form);

/* Finds the first (key ...) in options. Returns s7_nil when absent. */
s7_pointer	form_find(form_ctx_t* ctx, s7_pointer options, const char* key);

bool		form_is_absent(form_ctx_t* ctx, s7_pointer found);

/* Checks that options is a proper list of (key ...) forms, every key is in `allowed`,
 * and no key appears twice. `where` names the parent form in messages. */
bool		form_check_keys(form_ctx_t* ctx, s7_pointer options, const char* const* allowed,
				u32 count, const char* where);

/* Fails if `key` is missing from options. */
bool		form_need(form_ctx_t* ctx, s7_pointer options, const char* key, const char* where);

/* Value readers for a single (key value) form. */
bool		form_get_number(form_ctx_t* ctx, s7_pointer form, f64* out);
bool		form_get_int(form_ctx_t* ctx, s7_pointer form, s64 min, s64 max, s64* out);
bool		form_get_string(form_ctx_t* ctx, s7_pointer form, const char** out);
bool		form_get_bool(form_ctx_t* ctx, s7_pointer form, bool* out);
bool		form_get_choice(form_ctx_t* ctx, s7_pointer form, const char* const* choices,
				u32 count, u32* out);

/* Same readers, but looked up by key in options and optional. */
bool		form_opt_number(form_ctx_t* ctx, s7_pointer options, const char* key, f64* out);
bool		form_opt_int(form_ctx_t* ctx, s7_pointer options, const char* key,
			     s64 min, s64 max, s64* out);
bool		form_opt_string(form_ctx_t* ctx, s7_pointer options, const char* key, const char** out);
bool		form_opt_bool(form_ctx_t* ctx, s7_pointer options, const char* key, bool* out);
bool		form_opt_choice(form_ctx_t* ctx, s7_pointer options, const char* key,
				const char* const* choices, u32 count, u32* out);

#endif
