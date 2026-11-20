#include "device/fault.h"

fault_decision_t
fault_decide(const fault_t* fault, rng_t* rng)
{
	fault_decision_t	none = { 0 };

	if (fault->kind == FAULT_NONE)
		return none;

	if (!rng_percent(rng, fault->percent))
		return none;

	return (fault_decision_t){
		.drop     = (fault->kind == FAULT_DROP),
		.delay_ms = (fault->kind == FAULT_DELAY) ? fault->delay_ms : 0,
	};
}
