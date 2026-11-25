#include "device/fault.h"

enum { FAULT_MAX_DELAY_MS = 60000 };

static void
schedule_advance(fault_schedule_state_t* state, const fault_t* fault, u64 from_ms)
{
	state->step++;
	state->remaining = 0;
	if (state->step < fault->schedule_count)
		state->next_at_ms = from_ms + fault->schedule[state->step].after_ms;
}

static bool
schedule_fires(const fault_t* fault, fault_schedule_state_t* state, u64 now_ms, u64 start_ms)
{
	if (state->schedule_id != fault->schedule_id ||
	    state->source != fault->source ||
	    state->origin_ms != fault->origin_ms)
	{
		state->schedule_id = fault->schedule_id;
		state->source      = fault->source;
		state->origin_ms   = fault->origin_ms;
		if (fault->source != 0)
			start_ms = fault->origin_ms;
		state->step        = 0;
		state->remaining   = 0;
		state->next_at_ms  = start_ms;
		if (fault->schedule_count > 0)
			state->next_at_ms += fault->schedule[0].after_ms;
	}
	if (fault->schedule_id == 0)
		return false;
	if (state->remaining > 0)
	{
		state->remaining--;
		if (state->remaining == 0)
			schedule_advance(state, fault, now_ms);
		return true;
	}
	while (state->step < fault->schedule_count && now_ms >= state->next_at_ms)
	{
		if (fault->schedule[state->step].count == 0)
		{
			schedule_advance(state, fault, now_ms);
			continue;
		}
		state->remaining = fault->schedule[state->step].count;
		state->remaining--;
		if (state->remaining == 0)
			schedule_advance(state, fault, now_ms);
		return true;
	}
	return false;
}

fault_decision_t
fault_decide_at(const fault_set_t* set, rng_t* rng, fault_runtime_t* runtime,
		u64 now_ms, u64 start_ms)
{
	fault_decision_t	decision = { 0 };
	bool			fired[FAULT_KIND_COUNT] = { 0 };

	for (u32 i = 0; i < FAULT_KIND_COUNT; i++)
	{
		const fault_t*		fault = &set->by_kind[i];
		fault_schedule_state_t*	state = &runtime->by_kind[i];

		if ((set->present & FAULT_BIT(i)) == 0)
		{
			if (state->schedule_id != 0)
				*state = (fault_schedule_state_t){ 0 };
			continue;
		}
		if (fault->schedule_id == 0)
		{
			if (state->schedule_id != 0)
				*state = (fault_schedule_state_t){ 0 };
			fired[i] = rng_percent(rng, fault->percent);
			continue;
		}
		fired[i] = schedule_fires(fault, state, now_ms, start_ms);
	}

	decision.disconnect        = fired[FAULT_DISCONNECT];
	decision.restart           = fired[FAULT_RESTART];
	decision.sensor_disconnect = fired[FAULT_SENSOR_DISCONNECT];
	decision.drop              = fired[FAULT_DROP] || decision.disconnect;
	if (fired[FAULT_CORRUPT])
	{
		decision.corrupt = set->by_kind[FAULT_CORRUPT].corrupt_mode;
		if (decision.corrupt == FAULT_CORRUPT_NONE)
			decision.corrupt = FAULT_CORRUPT_PDU;
	}
	if (fired[FAULT_EXCEPTION])
		decision.exception_code = set->by_kind[FAULT_EXCEPTION].exception_code;
	for (u32 i = 0; i < FAULT_KIND_COUNT; i++)
	{
		if (!fired[i])
			continue;
		decision.delay_ms += set->by_kind[i].delay_ms;
		if (decision.delay_ms > FAULT_MAX_DELAY_MS)
			decision.delay_ms = FAULT_MAX_DELAY_MS;
	}
	return decision;
}
