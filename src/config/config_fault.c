#include "config/config_fault.h"
#include "vlib/macros.h"
#include <stddef.h>

/* Names the user writes, in the same order as the matching fault kinds. */
static const char* const KIND_NAMES[] = {
	"delay", "drop", "exception", "corrupt", "disconnect",
};
static const fault_kind_t KIND_VALUES[] = {
	FAULT_DELAY, FAULT_DROP, FAULT_EXCEPTION, FAULT_CORRUPT, FAULT_DISCONNECT,
};

bool
config_parse_fault(form_ctx_t* ctx, s7_pointer item, fault_set_t* set, const char* owner)
{
	static const char* const keys[] = { "kind", "delay-ms", "code", "percent" };
	s7_pointer	options = s7_cdr(item);
	s7_pointer	delay_f = NULL;
	s7_pointer	code_f  = NULL;
	u32		kind    = 0;
	fault_t*	fault   = NULL;
	s64		delay   = 0;
	s64		code    = 0;
	s64		percent = 100;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "fault"))
		return false;
	if (!form_need(ctx, options, "kind", "fault"))
		return false;
	if (!form_opt_choice(ctx, options, "kind", KIND_NAMES, ARRAY_COUNT(KIND_NAMES), &kind))
		return false;
	if ((set->present & FAULT_BIT(KIND_VALUES[kind])) != 0)
		return form_fail(ctx, "%s: %s already has a (fault (kind %s) ...); only one is allowed per kind",
				 form_text(ctx, item), owner, KIND_NAMES[kind]);

	delay_f = form_find(ctx, options, "delay-ms");
	code_f  = form_find(ctx, options, "code");
	if (kind == FAULT_DELAY && !form_need(ctx, options, "delay-ms", "fault with kind delay"))
		return false;
	if (kind != FAULT_DELAY && !form_is_absent(ctx, delay_f))
		return form_fail(ctx, "%s: delay-ms is only valid for kind delay", form_text(ctx, delay_f));
	if (kind == FAULT_EXCEPTION && !form_need(ctx, options, "code", "fault with kind exception"))
		return false;
	if (kind != FAULT_EXCEPTION && !form_is_absent(ctx, code_f))
		return form_fail(ctx, "%s: code is only valid for kind exception", form_text(ctx, code_f));

	if (kind == FAULT_DELAY && !form_get_int(ctx, delay_f, 1, 60000, &delay))
		return false;
	if (kind == FAULT_EXCEPTION)
	{
		if (!form_get_int(ctx, code_f, 1, 11, &code))
			return false;
		/* Standard codes 01-08, 0A, 0B; 09 is reserved. */
		if (code == 9)
			return form_fail(ctx, "%s: code 9 is reserved", form_text(ctx, code_f));
	}
	if (!form_opt_int(ctx, options, "percent", 0, 100, &percent))
		return false;

	fault = &set->by_kind[KIND_VALUES[kind]];
	*fault = (fault_t){
		.delay_ms       = (u32)delay,
		.exception_code = (u8)code,
		.percent        = (u32)percent,
	};
	set->present |= FAULT_BIT(KIND_VALUES[kind]);
	return true;
}
