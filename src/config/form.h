#ifndef FORM_H
#define FORM_H

/**
 * @file
 * @brief Helpers to read the config forms that s7 hands back.
 *
 * A config is plain data: lists like (port 1502) or (holding 0 (static 5)).
 * "options" below means the list of such (key value...) forms inside a parent form.
 * Every function reports problems in ctx->err in plain words and returns false.
 * The opt_* functions leave `out` untouched when the option is absent.
 */

#include "vlib/arena.h"
#include "vlib/c_types.h"
#include "third_party/s7/s7.h"
#include <stdbool.h>

typedef struct form_ctx_t form_ctx_t;

/** @brief Context shared by the config parsers: s7 scheme, error buffer and scratch arena. */
struct form_ctx_t
{
	s7_scheme*	sc;
	char*		err;
	u32		err_size;
	arena_t*	scratch;	/**< temporary storage for copied form text */
	void*		machine_state;	/**< opaque config_machine parser state */
};

/**
 * @brief Writes a printf-style message in ctx->err.
 *
 * Always returns false.
 */
bool		form_fail(form_ctx_t* ctx, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief Prints a form the way it was written, for error messages.
 *
 * Valid until the next s7 allocation.
 */
const char*	form_text(form_ctx_t* ctx, s7_pointer form);

/**
 * @brief Name of the first symbol of (name ...).
 *
 * NULL if form is not such a list.
 */
const char*	form_head_name(s7_pointer form);

/**
 * @brief Finds the first (key ...) in options.
 *
 * Returns s7_nil when absent.
 */
s7_pointer	form_find(form_ctx_t* ctx, s7_pointer options, const char* key);

bool		form_is_absent(form_ctx_t* ctx, s7_pointer found);

/**
 * @brief Checks that options is a proper list of (key ...) forms, every key is in `allowed`,
 *        and no key appears twice.
 *
 * `where` names the parent form in messages.
 */
bool		form_check_keys(form_ctx_t* ctx, s7_pointer options, const char* const* allowed,
				u32 count, const char* where);

/** @brief Fails if `key` is missing from options. */
bool		form_need(form_ctx_t* ctx, s7_pointer options, const char* key, const char* where);

/**
 * @name Readers for one (key value) form
 * Each returns false with a message in ctx->err when the value is missing or invalid.
 * @{
 */
bool		form_get_number(form_ctx_t* ctx, s7_pointer form, f64* out);
bool		form_get_int(form_ctx_t* ctx, s7_pointer form, s64 min, s64 max, s64* out);
bool		form_get_string(form_ctx_t* ctx, s7_pointer form, const char** out);
bool		form_get_bool(form_ctx_t* ctx, s7_pointer form, bool* out);
bool		form_get_choice(form_ctx_t* ctx, s7_pointer form, const char* const* choices,
				u32 count, u32* out);
/** @} */

/**
 * @name Readers for an optional key in options
 * Each leaves `out` untouched when the key is absent and returns false only for an invalid value.
 * @{
 */
bool		form_opt_number(form_ctx_t* ctx, s7_pointer options, const char* key, f64* out);
bool		form_opt_int(form_ctx_t* ctx, s7_pointer options, const char* key,
			     s64 min, s64 max, s64* out);
bool		form_opt_string(form_ctx_t* ctx, s7_pointer options, const char* key, const char** out);
bool		form_opt_bool(form_ctx_t* ctx, s7_pointer options, const char* key, bool* out);
bool		form_opt_choice(form_ctx_t* ctx, s7_pointer options, const char* key,
				const char* const* choices, u32 count, u32* out);
/** @} */

#endif
