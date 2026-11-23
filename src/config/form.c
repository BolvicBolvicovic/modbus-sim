#include "config/form.h"
#include "vlib/macros.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool
form_fail(form_ctx_t* ctx, const char* fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(ctx->err, ctx->err_size, fmt, ap);
	va_end(ap);
	return false;
}

const char*
form_text(form_ctx_t* ctx, s7_pointer form)
{
	return s7_string(s7_object_to_string(ctx->sc, form, true));
}

const char*
form_head_name(s7_pointer form)
{
	if (!s7_is_pair(form))
		return NULL;

	if (!s7_is_symbol(s7_car(form)))
		return NULL;

	return s7_symbol_name(s7_car(form));
}

s7_pointer
form_find(form_ctx_t* ctx, s7_pointer options, const char* key)
{
	for (s7_pointer p = options; s7_is_pair(p); p = s7_cdr(p))
	{
		const char*	head = form_head_name(s7_car(p));

		if (head && strcmp(head, key) == 0)
			return s7_car(p);
	}
	return s7_nil(ctx->sc);
}

bool
form_is_absent(form_ctx_t* ctx, s7_pointer found)
{
	return s7_is_null(ctx->sc, found);
}

static bool
key_is_allowed(const char* key, const char* const* allowed, u32 count)
{
	for (u32 i = 0; i < count; i++)
	{
		if (strcmp(key, allowed[i]) == 0)
			return true;
	}
	return false;
}

static u32
key_count(s7_pointer options, const char* key)
{
	u32	found = 0;

	for (s7_pointer p = options; s7_is_pair(p); p = s7_cdr(p))
	{
		const char*	head = form_head_name(s7_car(p));

		if (head && strcmp(head, key) == 0)
			found++;
	}
	return found;
}

static void
join_keys(char* out, u32 out_size, const char* const* keys, u32 count)
{
	u32	used = 0;

	out[0] = '\0';
	for (u32 i = 0; i < count && used + 1 < out_size; i++)
		used += (u32)snprintf(out + used, out_size - used, "%s%s", i ? ", " : "", keys[i]);
}

bool
form_check_keys(form_ctx_t* ctx, s7_pointer options, const char* const* allowed,
		u32 count, const char* where)
{
	char		list[256];
	s7_pointer	p = options;

	for (; s7_is_pair(p); p = s7_cdr(p))
	{
		const char*	head = form_head_name(s7_car(p));

		if (!head)
			return form_fail(ctx, "%s: expected an option like (name value), got %s",
					 where, form_text(ctx, s7_car(p)));

		join_keys(list, sizeof(list), allowed, count);
		if (!key_is_allowed(head, allowed, count))
			return form_fail(ctx, "%s: unknown option '%s'. Allowed: %s", where, head, list);

		if (key_count(options, head) > 1)
			return form_fail(ctx, "%s: option '%s' is given more than once", where, head);
	}

	if (!s7_is_null(ctx->sc, p))
		return form_fail(ctx, "%s: malformed list (stray dot?)", where);

	return true;
}

bool
form_need(form_ctx_t* ctx, s7_pointer options, const char* key, const char* where)
{
	if (form_is_absent(ctx, form_find(ctx, options, key)))
		return form_fail(ctx, "%s: missing required option (%s ...)", where, key);

	return true;
}

/* A value form must look like (key value). */
static bool
value_form_ok(form_ctx_t* ctx, s7_pointer form)
{
	if (s7_list_length(ctx->sc, form) == 2)
		return true;

	return form_fail(ctx, "%s: expected exactly one value, like (%s value)",
			 form_text(ctx, form), form_head_name(form));
}

bool
form_get_number(form_ctx_t* ctx, s7_pointer form, f64* out)
{
	s7_pointer	value = NULL;

	if (!value_form_ok(ctx, form))
		return false;

	value = s7_cadr(form);
	if (!s7_is_real(value))
		return form_fail(ctx, "%s: expected a number", form_text(ctx, form));

	*out = (f64)s7_real(value);
	return true;
}

bool
form_get_int(form_ctx_t* ctx, s7_pointer form, s64 min, s64 max, s64* out)
{
	s7_pointer	value = NULL;
	s64		number = 0;

	if (!value_form_ok(ctx, form))
		return false;

	value = s7_cadr(form);
	if (!s7_is_integer(value))
		return form_fail(ctx, "%s: expected a whole number", form_text(ctx, form));

	number = (s64)s7_integer(value);
	if (number < min || number > max)
		return form_fail(ctx, "%s: must be between %lld and %lld",
				 form_text(ctx, form), (long long)min, (long long)max);

	*out = number;
	return true;
}

bool
form_get_string(form_ctx_t* ctx, s7_pointer form, const char** out)
{
	s7_pointer	value = NULL;

	if (!value_form_ok(ctx, form))
		return false;

	value = s7_cadr(form);
	if (!s7_is_string(value))
		return form_fail(ctx, "%s: expected text in double quotes", form_text(ctx, form));

	*out = s7_string(value);
	return true;
}

/* Words people naturally write for on/off. */
static bool
symbol_to_bool(const char* name, bool* out)
{
	static const char* const yes[] = { "on", "yes", "true" };
	static const char* const no[]  = { "off", "no", "false" };

	for (u32 i = 0; i < ARRAY_COUNT(yes); i++)
	{
		if (strcmp(name, yes[i]) == 0) { *out = true;  return true; }
		if (strcmp(name, no[i]) == 0)  { *out = false; return true; }
	}
	return false;
}

bool
form_get_bool(form_ctx_t* ctx, s7_pointer form, bool* out)
{
	s7_pointer	value = NULL;

	if (!value_form_ok(ctx, form))
		return false;

	value = s7_cadr(form);
	if (s7_is_boolean(value))
	{
		*out = s7_boolean(ctx->sc, value);
		return true;
	}

	if (s7_is_integer(value) && (s7_integer(value) == 0 || s7_integer(value) == 1))
	{
		*out = s7_integer(value) == 1;
		return true;
	}

	if (s7_is_symbol(value) && symbol_to_bool(s7_symbol_name(value), out))
		return true;

	return form_fail(ctx, "%s: expected on or off (also yes/no, true/false, 1/0)",
			 form_text(ctx, form));
}

bool
form_get_choice(form_ctx_t* ctx, s7_pointer form, const char* const* choices,
		u32 count, u32* out)
{
	char		list[256];
	s7_pointer	value = NULL;

	if (!value_form_ok(ctx, form))
		return false;

	value = s7_cadr(form);
	join_keys(list, sizeof(list), choices, count);
	if (!s7_is_symbol(value))
		return form_fail(ctx, "%s: expected one of: %s", form_text(ctx, form), list);

	for (u32 i = 0; i < count; i++)
	{
		if (strcmp(s7_symbol_name(value), choices[i]) != 0)
			continue;
		*out = i;
		return true;
	}

	return form_fail(ctx, "%s: expected one of: %s", form_text(ctx, form), list);
}

bool
form_opt_number(form_ctx_t* ctx, s7_pointer options, const char* key, f64* out)
{
	s7_pointer	found = form_find(ctx, options, key);

	if (form_is_absent(ctx, found))
		return true;

	return form_get_number(ctx, found, out);
}

bool
form_opt_int(form_ctx_t* ctx, s7_pointer options, const char* key, s64 min, s64 max, s64* out)
{
	s7_pointer	found = form_find(ctx, options, key);

	if (form_is_absent(ctx, found))
		return true;

	return form_get_int(ctx, found, min, max, out);
}

bool
form_opt_string(form_ctx_t* ctx, s7_pointer options, const char* key, const char** out)
{
	s7_pointer	found = form_find(ctx, options, key);

	if (form_is_absent(ctx, found))
		return true;

	return form_get_string(ctx, found, out);
}

bool
form_opt_bool(form_ctx_t* ctx, s7_pointer options, const char* key, bool* out)
{
	s7_pointer	found = form_find(ctx, options, key);

	if (form_is_absent(ctx, found))
		return true;

	return form_get_bool(ctx, found, out);
}

bool
form_opt_choice(form_ctx_t* ctx, s7_pointer options, const char* key,
		const char* const* choices, u32 count, u32* out)
{
	s7_pointer	found = form_find(ctx, options, key);

	if (form_is_absent(ctx, found))
		return true;

	return form_get_choice(ctx, found, choices, count, out);
}
