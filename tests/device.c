#include "vtest.h"
#include "device/device.h"
#include "device/encode.h"
#include "device/fault.h"
#include "modbus/pdu.h"
#include "util/rng.h"
#include <math.h>
#include <string.h>
#include "vlib/macros.h"

static device_t*
fresh_device(arena_t* arena)
{
	return device_new(arena, 0);
}

static point_t
static_point(space_kind_t space, u32 address, value_type_t type, f64 value)
{
	point_t	p = { 0 };

	p.space = space;
	p.address = address;
	p.type = type;
	p.kind = POINT_STATIC;
	p.static_value = value;
	return p;
}

TEST(DeviceStoresStaticValue)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 10, VT_U16, 42);
	u16		out   = 0;

	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_EQ(device_read(dev, SPACE_HOLDING, 10, 1, &out));
	ASSERT_EQ(out == 42);
	arena_release(arena);
}

TEST(DeviceRejectsUndefinedAndOutOfRange)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 10, VT_U16, 1);
	u16		out[4];

	device_add_point(dev, &p);
	ASSERT_FAILURE(device_read(dev, SPACE_HOLDING, 9, 1, out));
	ASSERT_FAILURE(device_read(dev, SPACE_HOLDING, 10, 2, out));
	ASSERT_FAILURE(device_read(dev, SPACE_HOLDING, 65535, 2, out));
	ASSERT_FAILURE(device_read(dev, SPACE_INPUT, 10, 1, out));
	arena_release(arena);
}

TEST(DeviceRejectsOverlapAndWrongType)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		wide  = static_point(SPACE_INPUT, 0, VT_F32, 1.5);
	point_t		clash = static_point(SPACE_INPUT, 1, VT_U16, 1);
	point_t		bad   = static_point(SPACE_COIL, 0, VT_U16, 1);
	point_t		edge  = static_point(SPACE_INPUT, 65535, VT_U32, 1);

	ASSERT_SUCCESS(device_add_point(dev, &wide) == NULL);
	ASSERT_FAILURE(device_add_point(dev, &clash) == NULL);
	ASSERT_FAILURE(device_add_point(dev, &bad) == NULL);
	ASSERT_FAILURE(device_add_point(dev, &edge) == NULL);
	arena_release(arena);
}

TEST(DeviceRejectsBadRamp)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_INPUT, 0, VT_U16, 0);

	p.kind = POINT_RAMP;
	p.ramp = (ramp_params_t){ .from = 0, .to = 10, .step = 0, .every_ms = 1000 };
	ASSERT_FAILURE(device_add_point(dev, &p) == NULL);
	p.ramp.step = 1;
	p.ramp.every_ms = 0;
	ASSERT_FAILURE(device_add_point(dev, &p) == NULL);
	p.ramp.every_ms = 1000;
	ASSERT_SUCCESS(device_add_point(dev, &p) == NULL);
	arena_release(arena);
}

TEST(DeviceRejectsRampWithTooManySteps)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_INPUT, 0, VT_U16, 0);

	p.kind = POINT_RAMP;
	p.ramp = (ramp_params_t){ .from = 0, .to = 1e30, .step = 1, .every_ms = 1000 };
	ASSERT_FAILURE(device_add_point(dev, &p) == NULL);
	arena_release(arena);
}

TEST(DeviceTickMovesRamp)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_INPUT, 0, VT_U16, 0);
	u16		out   = 99;

	p.kind = POINT_RAMP;
	p.ramp = (ramp_params_t){ .from = 0, .to = 100, .step = 10, .every_ms = 1000, .mode = RAMP_HOLD };
	device_add_point(dev, &p);

	device_tick(dev, 3500);
	device_read(dev, SPACE_INPUT, 0, 1, &out);
	ASSERT_EQ(out == 30);
	arena_release(arena);
}

TEST(WritingFreezesRamp)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	u16		value = 500;
	u16		out   = 0;

	p.kind = POINT_RAMP;
	p.ramp = (ramp_params_t){ .from = 0, .to = 100, .step = 10, .every_ms = 1000, .mode = RAMP_WRAP };
	device_add_point(dev, &p);

	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, &value));
	device_tick(dev, 7000);
	device_read(dev, SPACE_HOLDING, 0, 1, &out);
	ASSERT_EQ(out == 500);
	arena_release(arena);
}

TEST(WriteToUndefinedAddressChangesNothing)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 0, VT_U16, 7);
	u16		values[2] = { 1, 2 };
	u16		out   = 0;

	device_add_point(dev, &p);
	ASSERT_FAILURE(device_write(dev, SPACE_HOLDING, 0, 2, values));
	device_read(dev, SPACE_HOLDING, 0, 1, &out);
	ASSERT_EQ(out == 7);
	arena_release(arena);
}

static void
add_fault(fault_set_t* set, fault_kind_t kind, u32 delay_ms, u8 code, u32 percent)
{
	set->by_kind[kind] = (fault_t){
		.delay_ms       = delay_ms,
		.exception_code = code,
		.percent        = percent,
	};
	set->present |= FAULT_BIT(kind);
}

static void
add_scheduled_fault(fault_set_t* set, fault_kind_t kind,
		    const fault_schedule_step_t* steps, u8 step_count, u64 schedule_id)
{
	fault_t* fault = &set->by_kind[kind];

	*fault = (fault_t){
		.schedule_count = step_count,
		.schedule_id    = schedule_id,
	};
	for (u8 i = 0; i < step_count; i++)
		fault->schedule[i] = steps[i];
	set->present |= FAULT_BIT(kind);
}

static fault_decision_t
decide(const fault_set_t* set, rng_t* rng)
{
	fault_runtime_t runtime = { 0 };

	return fault_decide_at(set, rng, &runtime, 0, 0);
}

TEST(FaultNoneNeverFires)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	rng_seed(&rng, 1);
	for (u32 i = 0; i < 1000; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(!d.drop && !d.disconnect && d.delay_ms == 0 &&
			  d.exception_code == 0 && !d.corrupt);
	}
}

TEST(FaultPercentIsRoughlyRespected)
{
	rng_t		rng;
	fault_set_t	set = { 0 };
	u32		dropped = 0;

	add_fault(&set, FAULT_DROP, 0, 0, 25);
	rng_seed(&rng, 99);
	for (u32 i = 0; i < 10000; i++)
		dropped += decide(&set, &rng).drop;

	ASSERT_EQ(dropped > 2200 && dropped < 2800);
}

TEST(FaultDelayAlwaysAtHundredPercent)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_DELAY, 250, 0, 100);
	rng_seed(&rng, 5);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(!d.drop && !d.disconnect && d.delay_ms == 250);
	}
}

TEST(FaultExceptionDecisionUsesCodeAtHundredPercent)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	rng_seed(&rng, 5);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.exception_code == EXC_SLAVE_DEVICE_BUSY);
		ASSERT_EQ(!d.drop && !d.disconnect && d.delay_ms == 0 && !d.corrupt);
	}

	set.by_kind[FAULT_EXCEPTION].percent = 0;
	rng_seed(&rng, 5);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.exception_code == 0 && !d.corrupt);
	}
}

TEST(FaultCorruptDecisionUsesFlagAtHundredPercent)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_CORRUPT, 0, 0, 100);
	rng_seed(&rng, 7);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.corrupt && d.exception_code == 0);
		ASSERT_EQ(!d.drop && !d.disconnect && d.delay_ms == 0);
	}

	set.by_kind[FAULT_CORRUPT].percent = 0;
	rng_seed(&rng, 7);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(!d.corrupt && d.exception_code == 0);
	}
}

TEST(FaultDisconnectAlwaysImpliesDrop)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_DISCONNECT, 0, 0, 100);
	rng_seed(&rng, 8);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.disconnect && d.drop);
		ASSERT_EQ(d.delay_ms == 0 && d.exception_code == 0 && !d.corrupt);
	}

	set.by_kind[FAULT_DISCONNECT].percent = 0;
	rng_seed(&rng, 8);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(!d.disconnect && !d.drop);
	}
}

TEST(FaultDelayAndCorruptCombine)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_DELAY, 200, 0, 100);
	add_fault(&set, FAULT_CORRUPT, 0, 0, 100);
	rng_seed(&rng, 11);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.delay_ms == 200 && d.corrupt);
		ASSERT_EQ(!d.drop && !d.disconnect && d.exception_code == 0);
	}
}

TEST(FaultExceptionAndCorruptCombine)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	add_fault(&set, FAULT_CORRUPT, 0, 0, 100);
	rng_seed(&rng, 12);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.exception_code == EXC_SLAVE_DEVICE_BUSY && d.corrupt);
		ASSERT_EQ(!d.drop && !d.disconnect);
	}
}

TEST(FaultDropAndExceptionCombine)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_DROP, 0, 0, 100);
	add_fault(&set, FAULT_EXCEPTION, 0, EXC_MEMORY_PARITY_ERROR, 100);
	rng_seed(&rng, 13);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.drop && d.exception_code == EXC_MEMORY_PARITY_ERROR);
		ASSERT_EQ(!d.disconnect && !d.corrupt);
	}
}

TEST(FaultDelaysAreCapped)
{
	rng_t		rng;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_DELAY, 60000, 0, 100);
	add_fault(&set, FAULT_CORRUPT, 59000, 0, 100);
	rng_seed(&rng, 14);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = decide(&set, &rng);

		ASSERT_EQ(d.delay_ms == 60000 && d.corrupt);
	}
}

TEST(FaultDecisionsAreDeterministicForSeed)
{
	rng_t		first;
	rng_t		second;
	fault_set_t	set = { 0 };

	add_fault(&set, FAULT_EXCEPTION, 0, EXC_MEMORY_PARITY_ERROR, 40);
	rng_seed(&first, 1234);
	rng_seed(&second, 1234);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	a = decide(&set, &first);
		fault_decision_t	b = decide(&set, &second);

		ASSERT_EQ(a.exception_code == b.exception_code);
	}

	set.present = 0;
	add_fault(&set, FAULT_CORRUPT, 0, 0, 40);
	rng_seed(&first, 4321);
	rng_seed(&second, 4321);
	for (u32 i = 0; i < 100; i++)
		ASSERT_EQ(decide(&set, &first).corrupt == decide(&set, &second).corrupt);

	set.present = 0;
	add_fault(&set, FAULT_DELAY, 100, 0, 50);
	add_fault(&set, FAULT_DROP, 0, 0, 50);
	add_fault(&set, FAULT_DISCONNECT, 0, 0, 50);
	rng_seed(&first, 5678);
	rng_seed(&second, 5678);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	a = decide(&set, &first);
		fault_decision_t	b = decide(&set, &second);

		ASSERT_EQ(a.drop == b.drop && a.disconnect == b.disconnect);
		ASSERT_EQ(a.delay_ms == b.delay_ms);
	}
}

TEST(FaultCorruptModeDefaultsToPduAndReportsConfiguredMode)
{
	rng_t			rng;
	fault_set_t		set = { 0 };
	fault_decision_t	d = { 0 };

	add_fault(&set, FAULT_CORRUPT, 0, 0, 100);
	rng_seed(&rng, 1);
	d = decide(&set, &rng);
	ASSERT_EQ(d.corrupt == FAULT_CORRUPT_PDU);

	set.by_kind[FAULT_CORRUPT].corrupt_mode = FAULT_CORRUPT_REGISTER;
	d = decide(&set, &rng);
	ASSERT_EQ(d.corrupt == FAULT_CORRUPT_REGISTER);

	set.by_kind[FAULT_CORRUPT].corrupt_mode = FAULT_CORRUPT_CHECKSUM;
	d = decide(&set, &rng);
	ASSERT_EQ(d.corrupt == FAULT_CORRUPT_CHECKSUM);

	set.by_kind[FAULT_CORRUPT].percent = 0;
	d = decide(&set, &rng);
	ASSERT_EQ(d.corrupt == FAULT_CORRUPT_NONE);
}

TEST(FaultRestartAndSensorDisconnectDoNotDrop)
{
	rng_t			rng;
	fault_set_t		set = { 0 };
	fault_decision_t	d = { 0 };

	add_fault(&set, FAULT_RESTART, 0, 0, 100);
	rng_seed(&rng, 2);
	d = decide(&set, &rng);
	ASSERT_EQ(d.restart && !d.sensor_disconnect);
	ASSERT_EQ(!d.drop && !d.disconnect && d.delay_ms == 0);
	ASSERT_EQ(d.exception_code == 0 && d.corrupt == FAULT_CORRUPT_NONE);

	set.present = 0;
	add_fault(&set, FAULT_SENSOR_DISCONNECT, 0, 0, 100);
	d = decide(&set, &rng);
	ASSERT_EQ(d.sensor_disconnect && !d.restart);
	ASSERT_EQ(!d.drop && !d.disconnect && d.delay_ms == 0);
	ASSERT_EQ(d.exception_code == 0 && d.corrupt == FAULT_CORRUPT_NONE);
}

TEST(FaultScheduleFiresExactStepCountsFromStart)
{
	rng_t				rng;
	fault_set_t			set = { 0 };
	fault_runtime_t			runtime = { 0 };
	const fault_schedule_step_t	steps[] = {
		{ .after_ms = 100, .count = 2 },
		{ .after_ms = 50,  .count = 1 },
	};
	const u64			start = 1000;

	add_scheduled_fault(&set, FAULT_DROP, steps, ARRAY_COUNT(steps), 1);
	rng_seed(&rng, 3);

	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, start + 99, start).drop);
	ASSERT_EQ(fault_decide_at(&set, &rng, &runtime, start + 100, start).drop);
	ASSERT_EQ(fault_decide_at(&set, &rng, &runtime, start + 101, start).drop);
	/* The second step waits after the request that completed the first one. */
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, start + 150, start).drop);
	ASSERT_EQ(fault_decide_at(&set, &rng, &runtime, start + 151, start).drop);
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, start + 152, start).drop);
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, start + 5000, start).drop);
}

TEST(FaultScheduleResetsOnKindAbsenceAndDefinitionChange)
{
	rng_t				rng;
	fault_set_t			set = { 0 };
	fault_runtime_t			runtime = { 0 };
	const fault_schedule_step_t	steps[] = { { .after_ms = 100, .count = 3 } };
	const u64			start = 1000;
	fault_schedule_state_t*		state = &runtime.by_kind[FAULT_DROP];

	add_scheduled_fault(&set, FAULT_DROP, steps, ARRAY_COUNT(steps), 7);
	rng_seed(&rng, 4);
	ASSERT_EQ(fault_decide_at(&set, &rng, &runtime, start + 100, start).drop);
	ASSERT_EQ(state->remaining == 2);

	/* Removing the kind clears its progress; re-adding arms from start again. */
	set.present &= (u8)~FAULT_BIT(FAULT_DROP);
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, start + 100, start).drop);
	ASSERT_EQ(state->schedule_id == 0 && state->step == 0 && state->remaining == 0);
	set.present |= FAULT_BIT(FAULT_DROP);
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, start + 99, start).drop);
	ASSERT_EQ(fault_decide_at(&set, &rng, &runtime, start + 100, start).drop);
	ASSERT_EQ(state->remaining == 2);

	/* A changed definition id restarts that kind's progress. */
	set.by_kind[FAULT_DROP].schedule_id = 8;
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, start + 50, start).drop);
	ASSERT_EQ(state->schedule_id == 8 && state->remaining == 0);
	ASSERT_EQ(fault_decide_at(&set, &rng, &runtime, start + 100, start).drop);
}

TEST(FaultScheduleOverridesPercent)
{
	rng_t				rng;
	fault_set_t			set = { 0 };
	fault_runtime_t			runtime = { 0 };
	const fault_schedule_step_t	steps[] = { { .after_ms = 100, .count = 1 } };
	const u64			start = 0;

	add_scheduled_fault(&set, FAULT_DROP, steps, ARRAY_COUNT(steps), 9);
	set.by_kind[FAULT_DROP].percent = 100;
	rng_seed(&rng, 5);
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, 99, start).drop);
	ASSERT_EQ(fault_decide_at(&set, &rng, &runtime, 100, start).drop);

	set.by_kind[FAULT_DROP].percent = 0;
	ASSERT_EQ(!fault_decide_at(&set, &rng, &runtime, 101, start).drop);
}

TEST(DeviceRestartRestoresPointsAndClearsScheduleRuntime)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 1000);
	point_t		static_p = static_point(SPACE_HOLDING, 0, VT_U16, 10);
	point_t		ramp_p   = static_point(SPACE_INPUT, 1, VT_U16, 0);
	u16			written = 99;
	u16			out     = 0;
	const fault_schedule_step_t	steps[] = { { .after_ms = 0, .count = 2 } };

	ramp_p.kind = POINT_RAMP;
	ramp_p.ramp = (ramp_params_t){
		.from = 0, .to = 100, .step = 10, .every_ms = 1000, .mode = RAMP_HOLD,
	};
	ASSERT_SUCCESS(device_add_point(dev, &static_p) == NULL);
	ASSERT_SUCCESS(device_add_point(dev, &ramp_p) == NULL);
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, &written));
	device_tick(dev, 2000);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 1, 1, &out));
	ASSERT_EQ(out == 10);

	add_scheduled_fault(&dev->faults, FAULT_DROP, steps, ARRAY_COUNT(steps), 11);
	{
		rng_t rng;

		rng_seed(&rng, 6);
		ASSERT_EQ(fault_decide_at(&dev->faults, &rng, &dev->fault_runtime,
					  2000, dev->start_ms).drop);
		ASSERT_EQ(fault_decide_at(&dev->faults, &rng, &dev->fault_runtime,
					  2000, dev->start_ms).drop);
	}
	dev->message_count = 7;
	dev->diag_exception_count = 3;

	device_restart(dev, 5000);
	ASSERT_EQ(dev->start_ms == 5000 && dev->now_ms == 5000);
	ASSERT_EQ(dev->message_count == 7 && dev->diag_exception_count == 3);
	for (u32 i = 0; i < FAULT_KIND_COUNT; i++)
	{
		ASSERT_EQ(dev->fault_runtime.by_kind[i].schedule_id == 0);
		ASSERT_EQ(dev->fault_runtime.by_kind[i].step == 0);
		ASSERT_EQ(dev->fault_runtime.by_kind[i].remaining == 0);
	}

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 10);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 1, 1, &out));
	ASSERT_EQ(out == 0);
	device_tick(dev, 6000);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 1, 1, &out));
	ASSERT_EQ(out == 10);
	arena_release(arena);
}

TEST(DeviceRestartPreservesRestartScheduleProgressOnly)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	rng_t		rng;
	fault_decision_t d = { 0 };
	const fault_schedule_step_t restart_steps[] = { { .after_ms = 100, .count = 2 } };
	const fault_schedule_step_t sensor_steps[]  = { { .after_ms = 100, .count = 2 } };

	add_scheduled_fault(&dev->faults, FAULT_RESTART, restart_steps,
			    ARRAY_COUNT(restart_steps), 21);
	add_scheduled_fault(&dev->faults, FAULT_SENSOR_DISCONNECT, sensor_steps,
			    ARRAY_COUNT(sensor_steps), 22);
	rng_seed(&rng, 7);

	d = fault_decide_at(&dev->faults, &rng, &dev->fault_runtime, 100, dev->start_ms);
	ASSERT_EQ(d.restart && d.sensor_disconnect);
	device_restart(dev, 100);

	/* The restart kind keeps its progress: the second of two requests fires now. */
	d = fault_decide_at(&dev->faults, &rng, &dev->fault_runtime, 100, dev->start_ms);
	ASSERT_EQ(d.restart && !d.sensor_disconnect);
	device_restart(dev, 100);

	/* The restart schedule is finished and does not re-arm; the other kind waits again. */
	d = fault_decide_at(&dev->faults, &rng, &dev->fault_runtime, 199, dev->start_ms);
	ASSERT_EQ(!d.restart && !d.sensor_disconnect);
	d = fault_decide_at(&dev->faults, &rng, &dev->fault_runtime, 200, dev->start_ms);
	ASSERT_EQ(!d.restart && d.sensor_disconnect);
	arena_release(arena);
}

TEST(FileRecordStoreAndRead)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		data[] = { 0x1234, 0x5678, 0x0009 };
	u16		out[3] = { 0, 0, 0 };

	ASSERT_EQ(device_add_file_record(dev, 1, 0, 2, data) == NULL);
	ASSERT_EQ(device_add_file_record(dev, 1, 10, 1, data + 2) == NULL);
	ASSERT_SUCCESS(device_read_file_records(dev, 1, 0, 2, out));
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678);
	ASSERT_SUCCESS(device_read_file_records(dev, 1, 10, 1, out));
	ASSERT_EQ(out[0] == 0x0009);
	ASSERT_FAILURE(device_read_file_records(dev, 1, 2, 1, out));	/* undefined */
	ASSERT_FAILURE(device_read_file_records(dev, 2, 0, 1, out));	/* unknown file */
	arena_release(arena);
}

TEST(FileRecordRejectsOverlapAndBadRange)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		data[] = { 1, 2, 3 };

	ASSERT_EQ(device_add_file_record(dev, 4, 5, 2, data) == NULL);
	ASSERT_FAILURE(device_add_file_record(dev, 4, 6, 1, data) == NULL);	/* overlap */
	ASSERT_FAILURE(device_add_file_record(dev, 4, 0x270F, 2, data) == NULL);/* past 9999 */
	ASSERT_FAILURE(device_add_file_record(dev, 4, 0, 0, data) == NULL);
	arena_release(arena);
}

TEST(FileRecordReadSpansMultipleSegments)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		data1[] = { 0x1111, 0x2222 };
	u16		data2[] = { 0x3333, 0x4444 };
	u16		out[4]  = { 0, 0, 0, 0 };

	ASSERT_EQ(device_add_file_record(dev, 1, 0, 2, data1) == NULL);
	ASSERT_EQ(device_add_file_record(dev, 1, 2, 2, data2) == NULL);
	ASSERT_SUCCESS(device_read_file_records(dev, 1, 0, 4, out));
	ASSERT_EQ(out[0] == 0x1111 && out[1] == 0x2222 && out[2] == 0x3333 && out[3] == 0x4444);
	arena_release(arena);
}

TEST(FileRecordStorageLimits)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		value = 1;

	for (u32 i = 0; i < DEVICE_MAX_FILES; i++)
		ASSERT_EQ(device_add_file_record(dev, (u16)(i + 1), 0, 1, &value) == NULL);
	ASSERT_FAILURE(device_add_file_record(dev, 99, 0, 1, &value) == NULL);

	dev = fresh_device(arena);
	for (u32 i = 0; i < DEVICE_MAX_FILE_SEGMENTS; i++)
		ASSERT_EQ(device_add_file_record(dev, 1, (u16)(i * 2), 1, &value) == NULL);
	ASSERT_FAILURE(device_add_file_record(dev, 1, 200, 1, &value) == NULL);
	arena_release(arena);
}

TEST(FileRecordWriteAndReadBack)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		data[] = { 0x1111, 0x2222, 0x3333 };
	u16		next[] = { 0xAAAA, 0xBBBB };
	u16		out[3] = { 0, 0, 0 };

	ASSERT_EQ(device_add_file_record(dev, 1, 0, 3, data) == NULL);
	ASSERT_SUCCESS(device_write_file_records(dev, 1, 1, 2, next));
	ASSERT_SUCCESS(device_read_file_records(dev, 1, 0, 3, out));
	ASSERT_EQ(out[0] == 0x1111 && out[1] == 0xAAAA && out[2] == 0xBBBB);
	arena_release(arena);
}

TEST(FileRecordWriteRejectsUndefinedAndLeavesUnchanged)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		data[] = { 0x0102, 0x0304 };
	u16		next[] = { 0xAAAA, 0xBBBB };
	u16		out[2] = { 0, 0 };

	ASSERT_EQ(device_add_file_record(dev, 1, 0, 2, data) == NULL);
	ASSERT_FAILURE(device_write_file_records(dev, 2, 0, 1, next));	/* unknown file */
	ASSERT_FAILURE(device_write_file_records(dev, 1, 2, 1, next));	/* undefined record */
	ASSERT_FAILURE(device_write_file_records(dev, 1, 1, 2, next));	/* spans undefined */
	ASSERT_SUCCESS(device_read_file_records(dev, 1, 0, 2, out));
	ASSERT_EQ(out[0] == 0x0102 && out[1] == 0x0304);
	arena_release(arena);
}

TEST(FileRecordWriteSpansMultipleSegments)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		data1[] = { 0x1111, 0x2222 };
	u16		data2[] = { 0x3333, 0x4444 };
	u16		next[]  = { 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD };
	u16		out[4]  = { 0, 0, 0, 0 };

	ASSERT_EQ(device_add_file_record(dev, 1, 0, 2, data1) == NULL);
	ASSERT_EQ(device_add_file_record(dev, 1, 2, 2, data2) == NULL);
	ASSERT_SUCCESS(device_write_file_records(dev, 1, 0, 4, next));
	ASSERT_SUCCESS(device_read_file_records(dev, 1, 0, 4, out));
	ASSERT_EQ(out[0] == 0xAAAA && out[1] == 0xBBBB && out[2] == 0xCCCC && out[3] == 0xDDDD);
	arena_release(arena);
}

TEST(FileRecordCanWriteChecksDefinedRange)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u16		data1[] = { 0x1111, 0x2222 };
	u16		data2[] = { 0x3333, 0x4444 };

	ASSERT_EQ(device_add_file_record(dev, 1, 0, 2, data1) == NULL);
	ASSERT_EQ(device_add_file_record(dev, 1, 2, 2, data2) == NULL);
	ASSERT_SUCCESS(device_can_write_file_records(dev, 1, 0, 4));
	ASSERT_FAILURE(device_can_write_file_records(dev, 1, 1, 4));
	ASSERT_FAILURE(device_can_write_file_records(dev, 2, 0, 1));
	ASSERT_FAILURE(device_can_write_file_records(dev, 1, 0, 0));
	arena_release(arena);
}

TEST(FifoQueueStoreAndRead)
{
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = fresh_device(arena);
	u16		data[] = { 0x1234, 0x5678, 0x0003 };
	u16		none[] = { 0 };
	u16		out[3] = { 0, 0, 0 };
	u16		count  = 0xFFFF;

	ASSERT_EQ(device_add_fifo(dev, 10, 3, data) == NULL);
	ASSERT_EQ(device_add_fifo(dev, 20, 0, none) == NULL);
	ASSERT_SUCCESS(device_read_fifo(dev, 10, &count, out));
	ASSERT_EQ(count == 3);
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678 && out[2] == 0x0003);
	ASSERT_SUCCESS(device_read_fifo(dev, 20, &count, out));
	ASSERT_EQ(count == 0);
	ASSERT_FAILURE(device_read_fifo(dev, 11, &count, out));
	arena_release(arena);
}

TEST(FifoQueueReadIsNonDestructive)
{
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = fresh_device(arena);
	u16		data[] = { 0x0001, 0x0002 };
	u16		out[2] = { 0, 0 };
	u16		count  = 0;

	ASSERT_EQ(device_add_fifo(dev, 1, 2, data) == NULL);
	ASSERT_SUCCESS(device_read_fifo(dev, 1, &count, out));
	ASSERT_EQ(count == 2 && out[0] == 1 && out[1] == 2);
	out[0] = out[1] = 0;
	count = 0;
	ASSERT_SUCCESS(device_read_fifo(dev, 1, &count, out));
	ASSERT_EQ(count == 2 && out[0] == 1 && out[1] == 2);
	arena_release(arena);
}

TEST(FifoQueueRejectsDuplicateTooManyAndTooLarge)
{
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = fresh_device(arena);
	u16		data[DEVICE_MAX_FIFO_VALUES + 1];
	u16		one    = 1;

	for (u32 i = 0; i < ARRAY_COUNT(data); i++)
		data[i] = (u16)i;

	ASSERT_FAILURE(device_add_fifo(dev, 1, DEVICE_MAX_FIFO_VALUES + 1, data) == NULL);
	ASSERT_EQ(device_add_fifo(dev, 1, DEVICE_MAX_FIFO_VALUES, data) == NULL);
	ASSERT_FAILURE(device_add_fifo(dev, 1, 0, &one) == NULL);

	dev = fresh_device(arena);
	for (u32 i = 0; i < DEVICE_MAX_FIFOS; i++)
		ASSERT_EQ(device_add_fifo(dev, (u16)i, 1, &one) == NULL);
	ASSERT_FAILURE(device_add_fifo(dev, 99, 1, &one) == NULL);
	arena_release(arena);
}

TEST(CanopenObjectStoreFindReadAndWrite)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = fresh_device(arena);
	u8			data[] = { 0x11, 0x22, 0x33, 0x44 };
	u8			out[4] = { 0, 0, 0, 0 };
	u8			next[] = { 0xAA, 0xBB };
	const device_canopen_object_t* obj = NULL;

	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(data), data) == NULL);
	ASSERT_EQ(dev->canopen_object_count == 1);
	obj = device_find_canopen_object(dev, 5, 0x6041, 0);
	ASSERT_SUCCESS(obj != NULL);
	ASSERT_EQ(obj->node_id == 5 && obj->index == 0x6041 && obj->subindex == 0);
	ASSERT_EQ(obj->length == sizeof(data));
	ASSERT_EQ(memcmp(obj->data, data, sizeof(data)) == 0);
	ASSERT_SUCCESS(device_find_canopen_object(dev, 5, 0x6041, 1) == NULL);
	ASSERT_SUCCESS(device_find_canopen_object(dev, 6, 0x6041, 0) == NULL);

	ASSERT_SUCCESS(device_canopen_read(dev, 5, 0x6041, 0, 1, 2, out));
	ASSERT_EQ(out[0] == 0x22 && out[1] == 0x33 && out[2] == 0);
	ASSERT_SUCCESS(device_canopen_write(dev, 5, 0x6041, 0, 2, 2, next));
	ASSERT_SUCCESS(device_canopen_read(dev, 5, 0x6041, 0, 0, 4, out));
	ASSERT_EQ(out[0] == 0x11 && out[1] == 0x22 && out[2] == 0xAA && out[3] == 0xBB);
	arena_release(arena);
}

TEST(CanopenObjectReadWriteRejectOutOfRange)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u8		data[] = { 1, 2, 3 };
	u8		in[]   = { 9, 9, 9 };
	u8		out[3] = { 0, 0, 0 };

	ASSERT_EQ(device_add_canopen_object(dev, 1, 0, 0, sizeof(data), data) == NULL);
	ASSERT_FAILURE(device_canopen_read(dev, 2, 0, 0, 0, 1, out));
	ASSERT_FAILURE(device_canopen_write(dev, 2, 0, 0, 0, 1, in));
	ASSERT_FAILURE(device_canopen_read(dev, 1, 0, 0, 2, 2, out));
	ASSERT_FAILURE(device_canopen_write(dev, 1, 0, 0, 2, 2, in));
	ASSERT_FAILURE(device_canopen_read(dev, 1, 0, 0, 4, 0, out));
	ASSERT_SUCCESS(device_canopen_read(dev, 1, 0, 0, 2, 1, out));
	ASSERT_EQ(out[0] == 3);
	ASSERT_SUCCESS(device_canopen_write(dev, 1, 0, 0, 3, 0, in));	/* empty range at the end */
	ASSERT_SUCCESS(device_canopen_read(dev, 1, 0, 0, 0, 3, out));
	ASSERT_EQ(out[0] == 1 && out[1] == 2 && out[2] == 3);
	arena_release(arena);
}

TEST(CanopenObjectRejectsDuplicatesAndBounds)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	u8		data[DEVICE_MAX_CANOPEN_OBJECT_BYTES];
	u8		one   = 1;

	memset(data, 0x5A, sizeof(data));
	ASSERT_FAILURE(device_add_canopen_object(dev, 0, 0, 0, 1, &one) == NULL);
	ASSERT_FAILURE(device_add_canopen_object(dev, 128, 0, 0, 1, &one) == NULL);
	ASSERT_FAILURE(device_add_canopen_object(dev, 1, 0, 0, 0, &one) == NULL);
	ASSERT_FAILURE(device_add_canopen_object(dev, 1, 0, 0, DEVICE_MAX_CANOPEN_OBJECT_BYTES + 1, data) == NULL);
	ASSERT_EQ(device_add_canopen_object(dev, 1, 0, 0, DEVICE_MAX_CANOPEN_OBJECT_BYTES, data) == NULL);
	ASSERT_FAILURE(device_add_canopen_object(dev, 1, 0, 0, 1, &one) == NULL);	/* duplicate */
	ASSERT_EQ(device_add_canopen_object(dev, 1, 0, 1, 1, &one) == NULL);
	ASSERT_EQ(device_add_canopen_object(dev, 2, 0, 0, 1, &one) == NULL);

	dev = fresh_device(arena);
	for (u32 i = 0; i < DEVICE_MAX_CANOPEN_OBJECTS; i++)
		ASSERT_EQ(device_add_canopen_object(dev, 1, (u16)i, 0, 1, &one) == NULL);
	ASSERT_FAILURE(device_add_canopen_object(dev, 1, 1000, 0, 1, &one) == NULL);
	arena_release(arena);
}

TEST(EncodeFloat32PreservesNonFiniteAndMapsOutOfRange)
{
	u16	w[2] = { 0, 0 };
	u32	raw  = 0;
	f32	real = 0;

	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, NAN, w) == 2);
	raw = ((u32)w[0] << 16) | w[1];
	memcpy(&real, &raw, sizeof(real));
	ASSERT_EQ(isnan(real));

	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, INFINITY, w) == 2);
	ASSERT_EQ(w[0] == 0x7F80 && w[1] == 0x0000);
	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, -INFINITY, w) == 2);
	ASSERT_EQ(w[0] == 0xFF80 && w[1] == 0x0000);

	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, 1e39, w) == 2);
	ASSERT_EQ(w[0] == 0x7F80 && w[1] == 0x0000);
	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, -1e39, w) == 2);
	ASSERT_EQ(w[0] == 0xFF80 && w[1] == 0x0000);
}

static void
check_value_round_trip(value_type_t type, word_order_t order, f64 value)
{
	u16	words[2] = { 0, 0 };
	f64	decoded = 0;

	ASSERT_EQ(encode_value(type, order, value, words) == value_width(type));
	decoded = decode_value(type, order, words);
	if (isnan(value))
		ASSERT_EQ(isnan(decoded));
	else
		ASSERT_EQ(decoded == value);
}

TEST(DecodeValueRoundTripsEveryTypeAndWordOrder)
{
	static const f64 values[VT_BOOL + 1][4] = {
		{ 0, 1, 12345, 65535 },					/* VT_U16 */
		{ -32768, -1, 0, 32767 },					/* VT_S16 */
		{ 0, 123456789, 3735928559.0, 4294967295.0 },		/* VT_U32 */
		{ -2147483648.0, -123456789, 0, 2147483647.0 },		/* VT_S32 */
		{ -3.5, 0, 1.25, 12345.5 },					/* VT_F32 */
		{ 0, 1, 0, 1 },						/* VT_BOOL */
	};

	for (u32 type = 0; type <= VT_BOOL; type++)
	{
		for (u32 order = 0; order <= WORDS_LOW_FIRST; order++)
		{
			for (u32 i = 0; i < ARRAY_COUNT(values[0]); i++)
				check_value_round_trip((value_type_t)type, (word_order_t)order,
						       values[type][i]);
		}
	}
}

TEST(DecodeValueHandlesNonFiniteAndWordOrder)
{
	u16	words[2] = { 0x1234, 0x5678 };

	ASSERT_EQ(decode_value(VT_U32, WORDS_HIGH_FIRST, words) == 0x12345678u);
	ASSERT_EQ(decode_value(VT_S32, WORDS_HIGH_FIRST, words) == 0x12345678);
	words[0] = 0x5678;
	words[1] = 0x1234;
	ASSERT_EQ(decode_value(VT_U32, WORDS_LOW_FIRST, words) == 0x12345678u);
	ASSERT_EQ(decode_value(VT_S32, WORDS_LOW_FIRST, words) == 0x12345678);

	words[0] = 0xFFFF;
	words[1] = 0xFFFF;
	ASSERT_EQ(decode_value(VT_U16, WORDS_HIGH_FIRST, words) == 65535);
	ASSERT_EQ(decode_value(VT_S16, WORDS_HIGH_FIRST, words) == -1);
	ASSERT_EQ(decode_value(VT_U32, WORDS_HIGH_FIRST, words) == 4294967295u);
	ASSERT_EQ(decode_value(VT_S32, WORDS_HIGH_FIRST, words) == -1);
	ASSERT_EQ(decode_value(VT_BOOL, WORDS_HIGH_FIRST, words) == 1);
	words[0] = 0;
	ASSERT_EQ(decode_value(VT_BOOL, WORDS_HIGH_FIRST, words) == 0);

	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, NAN, words) == 2);
	ASSERT_EQ(isnan(decode_value(VT_F32, WORDS_HIGH_FIRST, words)));
	ASSERT_EQ(encode_value(VT_F32, WORDS_LOW_FIRST, INFINITY, words) == 2);
	ASSERT_EQ(isinf(decode_value(VT_F32, WORDS_LOW_FIRST, words)));
	ASSERT_EQ(decode_value(VT_F32, WORDS_LOW_FIRST, words) > 0);
	ASSERT_EQ(encode_value(VT_F32, WORDS_LOW_FIRST, -INFINITY, words) == 2);
	ASSERT_EQ(isinf(decode_value(VT_F32, WORDS_LOW_FIRST, words)));
	ASSERT_EQ(decode_value(VT_F32, WORDS_LOW_FIRST, words) < 0);
}

TEST(DevicePointAccessOnlyAtFirstAddress)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 10, VT_U32, 0x11223344u);
	val_t		in    = { .type = VAL_NUM, .n = 99 };
	val_t		out   = { 0 };
	u16		words[2];

	ASSERT_EQ(device_add_point(dev, &p) == NULL);

	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 10, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 0x11223344u);
	ASSERT_FAILURE(device_get_point(dev, SPACE_HOLDING, 11, &out));
	ASSERT_FAILURE(device_get_point(dev, SPACE_HOLDING, 9, &out));
	ASSERT_FAILURE(device_get_point(dev, SPACE_HOLDING, 65535, &out));
	ASSERT_FAILURE(device_get_point(dev, SPACE_INPUT, 10, &out));

	ASSERT_SUCCESS(device_set_point(dev, SPACE_HOLDING, 10, &in));
	ASSERT_FAILURE(device_set_point(dev, SPACE_HOLDING, 11, &in));
	ASSERT_FAILURE(device_set_point(dev, SPACE_HOLDING, 9, &in));
	ASSERT_FAILURE(device_set_point(dev, SPACE_INPUT, 10, &in));
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 10, 2, words));
	ASSERT_EQ(words[0] == 0 && words[1] == 99);
	arena_release(arena);
}

TEST(DeviceFindPointMatchesFirstAddressOnly)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		wide  = static_point(SPACE_HOLDING, 10, VT_U32, 0x11223344u);
	point_t		next  = static_point(SPACE_HOLDING, 12, VT_U16, 5);

	ASSERT_EQ(device_add_point(dev, &wide) == NULL);
	ASSERT_EQ(device_add_point(dev, &next) == NULL);
	ASSERT_EQ(device_find_point(dev, SPACE_HOLDING, 10) == 0);
	ASSERT_EQ(device_find_point(dev, SPACE_HOLDING, 12) == 1);
	ASSERT_EQ(device_find_point(dev, SPACE_HOLDING, 11) == DEVICE_MAX_POINTS);
	ASSERT_EQ(device_find_point(dev, SPACE_HOLDING, 9) == DEVICE_MAX_POINTS);
	ASSERT_EQ(device_find_point(dev, SPACE_INPUT, 10) == DEVICE_MAX_POINTS);
	ASSERT_EQ(device_find_point(dev, SPACE_HOLDING, ADDRESS_SPACE_SIZE) == DEVICE_MAX_POINTS);
	ASSERT_EQ(device_find_point(dev, (space_kind_t)SPACE_COUNT, 0) == DEVICE_MAX_POINTS);
	arena_release(arena);
}

TEST(DevicePointAccessEnforcesValueTypes)
{
	arena_t*	arena   = ARENA_ALLOC();
	device_t*	dev     = fresh_device(arena);
	point_t		coil    = static_point(SPACE_COIL, 1, VT_BOOL, 0);
	point_t		reg     = static_point(SPACE_HOLDING, 2, VT_U16, 7);
	val_t		num     = { .type = VAL_NUM, .n = 1 };
	val_t		boolean = { .type = VAL_BOOL, .b = true };
	val_t		bad     = { .type = (val_type_t)99, .n = 1 };
	val_t		out     = { 0 };

	ASSERT_EQ(device_add_point(dev, &coil) == NULL);
	ASSERT_EQ(device_add_point(dev, &reg) == NULL);

	ASSERT_SUCCESS(device_get_point(dev, SPACE_COIL, 1, &out));
	ASSERT_EQ(out.type == VAL_BOOL && !out.b && out.n == 0);
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 2, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 7 && !out.b);

	ASSERT_FAILURE(device_set_point(dev, SPACE_COIL, 1, &num));
	ASSERT_FAILURE(device_set_point(dev, SPACE_HOLDING, 2, &boolean));
	ASSERT_FAILURE(device_set_point(dev, SPACE_COIL, 1, &bad));
	ASSERT_FAILURE(device_set_point(dev, SPACE_HOLDING, 2, &bad));
	ASSERT_SUCCESS(device_set_point(dev, SPACE_COIL, 1, &boolean));
	ASSERT_SUCCESS(device_set_point(dev, SPACE_HOLDING, 2, &num));

	ASSERT_SUCCESS(device_get_point(dev, SPACE_COIL, 1, &out));
	ASSERT_EQ(out.type == VAL_BOOL && out.b);
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 2, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 1);
	arena_release(arena);
}

TEST(MasterWriteRejectsMachinePoint)
{
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = fresh_device(arena);
	point_t		p      = static_point(SPACE_HOLDING, 0, VT_U32, 5);
	val_t		in     = { .type = VAL_NUM, .n = 100 };
	val_t		out    = { 0 };
	u16		words[2] = { 1, 0 };

	p.kind = POINT_MACHINE;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_EQ(dev->points[0].kind == POINT_MACHINE);

	ASSERT_SUCCESS(device_set_point(dev, SPACE_HOLDING, 0, &in));
	ASSERT_EQ(dev->points[0].kind == POINT_MACHINE);
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 0, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 100);

	ASSERT_FAILURE(device_write(dev, SPACE_HOLDING, 0, 2, words));
	ASSERT_EQ(dev->points[0].kind == POINT_MACHINE);
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 0, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 100);
	device_tick(dev, 5000);
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 0, &out));
	ASSERT_EQ(out.type == VAL_NUM && out.n == 100);
	arena_release(arena);
}

TEST(MasterWriteRejectsRangeContainingMachinePoint)
{
	arena_t*	arena   = ARENA_ALLOC();
	device_t*	dev     = fresh_device(arena);
	point_t		ramp    = static_point(SPACE_HOLDING, 0, VT_U16, 0);
	point_t		machine = static_point(SPACE_HOLDING, 1, VT_U16, 0);
	point_t		fixed   = static_point(SPACE_HOLDING, 2, VT_U16, 0);
	u16		words[3] = { 500, 600, 700 };
	u16		out[3]   = { 0, 0, 0 };

	ramp.kind = POINT_RAMP;
	ramp.ramp = (ramp_params_t){ .from = 0, .to = 100, .step = 10,
				     .every_ms = 1000, .mode = RAMP_WRAP };
	machine.kind = POINT_MACHINE;
	ASSERT_EQ(device_add_point(dev, &ramp) == NULL);
	ASSERT_EQ(device_add_point(dev, &machine) == NULL);
	ASSERT_EQ(device_add_point(dev, &fixed) == NULL);

	ASSERT_FAILURE(device_write(dev, SPACE_HOLDING, 0, 3, words));
	ASSERT_EQ(dev->points[0].kind == POINT_RAMP);
	ASSERT_EQ(dev->points[1].kind == POINT_MACHINE);
	ASSERT_EQ(dev->points[2].kind == POINT_STATIC);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 3, out));
	ASSERT_EQ(out[0] == 0 && out[1] == 0 && out[2] == 0);

	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, words));
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 2, 1, &words[2]));
	ASSERT_EQ(dev->points[0].kind == POINT_STATIC);
	ASSERT_EQ(dev->points[1].kind == POINT_MACHINE);
	ASSERT_EQ(dev->points[2].kind == POINT_STATIC);

	device_tick(dev, 7000);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 3, out));
	ASSERT_EQ(out[0] == 500 && out[1] == 0 && out[2] == 700);
	arena_release(arena);
}
