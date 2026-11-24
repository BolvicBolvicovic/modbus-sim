#include "device/fault.h"

enum { FAULT_MAX_DELAY_MS = 60000 };

fault_decision_t
fault_decide(const fault_set_t* set, rng_t* rng)
{
	fault_decision_t	decision = { 0 };
	bool			fired[FAULT_KIND_COUNT] = { 0 };

	for (u32 i = 0; i < FAULT_KIND_COUNT; i++)
	{
		if ((set->present & FAULT_BIT(i)) != 0)
			fired[i] = rng_percent(rng, set->by_kind[i].percent);
	}

	decision.disconnect = fired[FAULT_DISCONNECT];
	decision.drop        = fired[FAULT_DROP] || decision.disconnect;
	decision.corrupt     = fired[FAULT_CORRUPT];
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
