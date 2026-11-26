/**
 * @file
 * @brief Parses (fault ...) forms and their schedules.
 */
#include "config/config_fault.h"
#include "vlib/macros.h"
#include <stddef.h>
#include <string.h>

/** @brief Names the user writes, in the same order as the matching fault kinds. */
static const char* const KIND_NAMES[] = {
	"delay", "drop", "exception", "corrupt", "disconnect", "restart", "sensor-disconnect",
};
static const fault_kind_t KIND_VALUES[] = {
	FAULT_DELAY, FAULT_DROP, FAULT_EXCEPTION, FAULT_CORRUPT, FAULT_DISCONNECT,
	FAULT_RESTART, FAULT_SENSOR_DISCONNECT,
};

/** @brief Names the user writes, in the same order as the matching corrupt modes. */
static const char* const CORRUPT_MODE_NAMES[] = { "pdu", "register", "checksum" };
static const fault_corrupt_mode_t CORRUPT_MODE_VALUES[] = {
	FAULT_CORRUPT_PDU, FAULT_CORRUPT_REGISTER, FAULT_CORRUPT_CHECKSUM,
};

static u64
schedule_hash_u32(u64 hash, u32 value)
{
	for (u32 i = 0; i < 4; i++)
	{
		hash ^= (u8)(value >> (i * 8));
		hash *= 1099511628211ULL; /* FNV-1a 64-bit prime */
	}
	return hash;
}

/**
 * @brief Parses (schedule (step (after-ms N) (count N)) ...) into `fault`.
 *
 * The stable schedule_id lets the runtime notice when the active definition changed.
 */
static bool
parse_fault_schedule(form_ctx_t* ctx, s7_pointer form, fault_t* fault)
{
	static const char* const keys[] = { "after-ms", "count" };
	s7_pointer		 p     = s7_cdr(form);
	u32			 count = 0;
	u64			 hash  = 14695981039346656037ULL; /* FNV-1a 64-bit offset basis */

	for (; s7_is_pair(p); p = s7_cdr(p))
	{
		s7_pointer	step = s7_car(p);
		s7_pointer	options;
		const char*	head = form_head_name(step);
		s64		after_ms = 0;
		s64		times    = 0;

		if (!head || strcmp(head, "step") != 0)
			return form_fail(ctx, "%s: expected (step (after-ms N) (count N))",
					 form_text(ctx, step));
		if (count >= FAULT_MAX_SCHEDULE_STEPS)
			return form_fail(ctx, "fault: schedule has more than %d steps",
					 (int)FAULT_MAX_SCHEDULE_STEPS);

		options = s7_cdr(step);
		if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "schedule step"))
			return false;
		if (!form_need(ctx, options, "after-ms", "schedule step") ||
		    !form_need(ctx, options, "count", "schedule step"))
			return false;
		if (!form_opt_int(ctx, options, "after-ms", 0, 86400000, &after_ms) ||
		    !form_opt_int(ctx, options, "count", 1, 1000000, &times))
			return false;

		fault->schedule[count] = (fault_schedule_step_t){
			.after_ms = (u32)after_ms,
			.count    = (u32)times,
		};
		count++;
	}

	if (!s7_is_null(ctx->sc, p))
		return form_fail(ctx, "fault: malformed schedule step list");
	if (count == 0)
		return form_fail(ctx, "%s: schedule needs at least one (step ...)", form_text(ctx, form));

	hash = schedule_hash_u32(hash, count);
	for (u32 i = 0; i < count; i++)
	{
		hash = schedule_hash_u32(hash, fault->schedule[i].after_ms);
		hash = schedule_hash_u32(hash, fault->schedule[i].count);
	}
	fault->schedule_count = (u8)count;
	fault->schedule_id    = hash ? hash : 1;
	return true;
}

bool
config_parse_fault(form_ctx_t* ctx, s7_pointer item, fault_set_t* set, const char* owner)
{
	static const char* const keys[] = { "kind", "delay-ms", "code", "percent", "mode", "schedule" };
	s7_pointer	options       = s7_cdr(item);
	s7_pointer	delay_f       = NULL;
	s7_pointer	code_f        = NULL;
	s7_pointer	mode_f        = NULL;
	s7_pointer	percent_f     = NULL;
	s7_pointer	schedule_f    = NULL;
	u32		kind          = 0;
	u32		corrupt_mode  = 0;
	fault_t*	fault         = NULL;
	s64		delay         = 0;
	s64		code          = 0;
	s64		percent       = 100;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "fault"))
		return false;
	if (!form_need(ctx, options, "kind", "fault"))
		return false;
	if (!form_opt_choice(ctx, options, "kind", KIND_NAMES, ARRAY_COUNT(KIND_NAMES), &kind))
		return false;
	if ((set->present & FAULT_BIT(KIND_VALUES[kind])) != 0)
		return form_fail(ctx, "%s: %s already has a (fault (kind %s) ...); only one is allowed per kind",
				 form_text(ctx, item), owner, KIND_NAMES[kind]);

	delay_f    = form_find(ctx, options, "delay-ms");
	code_f     = form_find(ctx, options, "code");
	mode_f     = form_find(ctx, options, "mode");
	percent_f  = form_find(ctx, options, "percent");
	schedule_f = form_find(ctx, options, "schedule");
	if (kind == FAULT_DELAY && !form_need(ctx, options, "delay-ms", "fault with kind delay"))
		return false;
	if (kind != FAULT_DELAY && !form_is_absent(ctx, delay_f))
		return form_fail(ctx, "%s: delay-ms is only valid for kind delay", form_text(ctx, delay_f));
	if (kind == FAULT_EXCEPTION && !form_need(ctx, options, "code", "fault with kind exception"))
		return false;
	if (kind != FAULT_EXCEPTION && !form_is_absent(ctx, code_f))
		return form_fail(ctx, "%s: code is only valid for kind exception", form_text(ctx, code_f));
	if (kind == FAULT_CORRUPT)
	{
		if (!form_opt_choice(ctx, options, "mode", CORRUPT_MODE_NAMES,
				     ARRAY_COUNT(CORRUPT_MODE_NAMES), &corrupt_mode))
			return false;
	}
	else if (!form_is_absent(ctx, mode_f))
	{
		return form_fail(ctx, "%s: mode is only valid for kind corrupt", form_text(ctx, mode_f));
	}
	if (!form_is_absent(ctx, schedule_f) && !form_is_absent(ctx, percent_f))
		return form_fail(ctx, "%s: percent cannot be combined with schedule",
				 form_text(ctx, percent_f));

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
		.corrupt_mode   = kind == FAULT_CORRUPT ? CORRUPT_MODE_VALUES[corrupt_mode]
							: FAULT_CORRUPT_NONE,
	};
	if (!form_is_absent(ctx, schedule_f) && !parse_fault_schedule(ctx, schedule_f, fault))
		return false;
	set->present |= FAULT_BIT(KIND_VALUES[kind]);
	return true;
}
