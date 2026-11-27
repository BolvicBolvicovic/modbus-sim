#include "vtest.h"
#include "device/device.h"
#include "device/encode.h"
#include "device/fault.h"
#include "modbus/pdu.h"
#include "util/rng.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
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

TEST(FaultPairsCombineInOneDecision)
{
	static const struct {
		const char*	name;
		fault_kind_t	a, b;
		u32		delay_ms;
		u8		code;
		bool		drop, corrupt;
	} cases[] = {
		{ "delay+corrupt",     FAULT_DELAY,     FAULT_CORRUPT,   200, 0, false, true },
		{ "exception+corrupt", FAULT_EXCEPTION, FAULT_CORRUPT,   0, EXC_SLAVE_DEVICE_BUSY, false, true },
		{ "drop+exception",    FAULT_DROP,      FAULT_EXCEPTION, 0, EXC_MEMORY_PARITY_ERROR, true, false },
	};

	for (u32 c = 0; c < ARRAY_COUNT(cases); c++)
	{
		rng_t		rng;
		fault_set_t	set = { 0 };

		fprintf(stderr, "fault pair '%s'\n", cases[c].name);
		for (u32 k = 0; k < 2; k++)
		{
			fault_kind_t kind = k == 0 ? cases[c].a : cases[c].b;

			add_fault(&set, kind, kind == FAULT_DELAY ? cases[c].delay_ms : 0,
				  kind == FAULT_EXCEPTION ? cases[c].code : 0, 100);
		}
		rng_seed(&rng, 11 + c);
		for (u32 i = 0; i < 100; i++)
		{
			fault_decision_t	d = decide(&set, &rng);

			ASSERT_EQ(d.delay_ms == cases[c].delay_ms);
			ASSERT_EQ(d.exception_code == cases[c].code);
			ASSERT_EQ(d.drop == cases[c].drop && d.corrupt == cases[c].corrupt);
			ASSERT_EQ(!d.disconnect);
		}
	}
}

TEST(EncodeFloat32PreservesNonFiniteAndMapsOutOfRange)
{
	static const struct {
		f64	in;
		u16	high;
	} cases[] = {
		{ INFINITY, 0x7F80 }, { -INFINITY, 0xFF80 },
		{ 1e39, 0x7F80 }, { -1e39, 0xFF80 },
		{ (f64)FLT_MAX * 2.0, 0x7F80 }, { -((f64)FLT_MAX * 2.0), 0xFF80 },
	};
	u16	w[4] = { 0, 0, 0, 0 };
	u32	raw  = 0;
	f32	real = 0;

	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, BYTES_BIG, NAN, w) == 2);
	raw = ((u32)w[0] << 16) | w[1];
	memcpy(&real, &raw, sizeof(real));
	ASSERT_EQ(isnan(real));

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, BYTES_BIG, cases[i].in, w) == 2);
		ASSERT_EQ(w[0] == cases[i].high && w[1] == 0x0000);
	}

	/* 1.0 = 0x3F800000 */
	encode_value(VT_F32, WORDS_HIGH_FIRST, BYTES_BIG, 1.0, w);
	ASSERT_EQ(w[0] == 0x3F80 && w[1] == 0x0000);
	encode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, 1.0, w);
	ASSERT_EQ(w[0] == 0x0000 && w[1] == 0x3F80);
}

TEST(EncodeIntegersClampRoundAndBoolIsOneBit)
{
	static const struct {
		value_type_t	type;
		f64		in;
		u16		w0, w1;
	} cases[] = {
		{ VT_U16, 70000, 65535, 0 },
		{ VT_S16, -2, 0xFFFE, 0 },
		{ VT_U16, 12.6, 13, 0 },
		{ VT_U16, NAN, 0, 0 },
		{ VT_S32, INFINITY, 0x7FFF, 0xFFFF },
		{ VT_U32, -INFINITY, 0x0000, 0x0000 },
		{ VT_BOOL, 5, 1, 0 },
		{ VT_BOOL, 0, 0, 0 },
	};

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		u16	w[4] = { 0, 0, 0, 0 };

		ASSERT_EQ(encode_value(cases[i].type, WORDS_HIGH_FIRST, BYTES_BIG, cases[i].in, w) ==
			  value_width(cases[i].type));
		ASSERT_EQ(w[0] == cases[i].w0 && w[1] == cases[i].w1);
	}
}

static void
check_value_round_trip(value_type_t type, word_order_t order, byte_order_t bytes, f64 value)
{
	u16	words[4] = { 0 };
	f64	decoded = 0;

	ASSERT_EQ(encode_value(type, order, bytes, value, words) == value_width(type));
	decoded = decode_value(type, order, bytes, words);
	if (isnan(value))
		ASSERT_EQ(isnan(decoded));
	else
		ASSERT_EQ(decoded == value);
}

TEST(DecodeValueRoundTripsEveryTypeWordOrderAndByteOrder)
{
	static const f64 values[VT_F64 + 1][4] = {
		{ 0, 1, 12345, 65535 },					/* VT_U16 */
		{ -32768, -1, 0, 32767 },					/* VT_S16 */
		{ 0, 123456789, 3735928559.0, 4294967295.0 },		/* VT_U32 */
		{ -2147483648.0, -123456789, 0, 2147483647.0 },		/* VT_S32 */
		{ -3.5, 0, 1.25, 12345.5 },					/* VT_F32 */
		{ 0, 1, 0, 1 },						/* VT_BOOL */
		{ 0, 4294967296.0, 9007199254740992.0, 1.5e19 },	/* VT_U64 */
		{ -9007199254740992.0, -1, 0, 4611686018427387904.0 },	/* VT_S64 */
		{ -3.5, 0, 1.0e300, NAN },					/* VT_F64 */
	};

	for (u32 type = 0; type <= VT_F64; type++)
	{
		for (u32 order = 0; order <= WORDS_LOW_FIRST; order++)
		{
			for (u32 bytes = 0; bytes <= BYTES_LITTLE; bytes++)
			{
				for (u32 i = 0; i < ARRAY_COUNT(values[0]); i++)
					check_value_round_trip((value_type_t)type, (word_order_t)order,
							       (byte_order_t)bytes, values[type][i]);
			}
		}
	}
}

TEST(EncodeValueMatchesTheFourCommonThirtyTwoBitLayouts)
{
	u16	w[4] = { 0 };

	/* 1.5f = 0x3FC00000: ABCD = 3F C0 00 00 */
	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, BYTES_BIG, 1.5, w) == 2);
	ASSERT_EQ(w[0] == 0x3FC0 && w[1] == 0x0000);
	ASSERT_EQ(encode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, 1.5, w) == 2);
	ASSERT_EQ(w[0] == 0x0000 && w[1] == 0x3FC0);
	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, BYTES_LITTLE, 1.5, w) == 2);
	ASSERT_EQ(w[0] == 0xC03F && w[1] == 0x0000);
	ASSERT_EQ(encode_value(VT_F32, WORDS_LOW_FIRST, BYTES_LITTLE, 1.5, w) == 2);
	ASSERT_EQ(w[0] == 0x0000 && w[1] == 0xC03F);

	/* 0x12345678: ABCD, CDAB, BADC, DCBA */
	ASSERT_EQ(encode_value(VT_U32, WORDS_HIGH_FIRST, BYTES_BIG, 0x12345678, w) == 2);
	ASSERT_EQ(w[0] == 0x1234 && w[1] == 0x5678);
	ASSERT_EQ(encode_value(VT_U32, WORDS_LOW_FIRST, BYTES_BIG, 0x12345678, w) == 2);
	ASSERT_EQ(w[0] == 0x5678 && w[1] == 0x1234);
	ASSERT_EQ(encode_value(VT_U32, WORDS_HIGH_FIRST, BYTES_LITTLE, 0x12345678, w) == 2);
	ASSERT_EQ(w[0] == 0x3412 && w[1] == 0x7856);
	ASSERT_EQ(encode_value(VT_U32, WORDS_LOW_FIRST, BYTES_LITTLE, 0x12345678, w) == 2);
	ASSERT_EQ(w[0] == 0x7856 && w[1] == 0x3412);

	/* 16-bit: only the byte order matters */
	ASSERT_EQ(encode_value(VT_U16, WORDS_LOW_FIRST, BYTES_LITTLE, 0x1234, w) == 1);
	ASSERT_EQ(w[0] == 0x3412);
}

TEST(EncodeBitsLaysOutSixtyFourBitValuesInEveryOrder)
{
	const u64	raw = 0x0102030405060708ull;
	u16		w[4] = { 0 };

	encode_bits(4, WORDS_HIGH_FIRST, BYTES_BIG, raw, w);
	ASSERT_EQ(w[0] == 0x0102 && w[1] == 0x0304 && w[2] == 0x0506 && w[3] == 0x0708);
	ASSERT_EQ(decode_bits(4, WORDS_HIGH_FIRST, BYTES_BIG, w) == raw);
	encode_bits(4, WORDS_LOW_FIRST, BYTES_BIG, raw, w);
	ASSERT_EQ(w[0] == 0x0708 && w[1] == 0x0506 && w[2] == 0x0304 && w[3] == 0x0102);
	ASSERT_EQ(decode_bits(4, WORDS_LOW_FIRST, BYTES_BIG, w) == raw);
	encode_bits(4, WORDS_HIGH_FIRST, BYTES_LITTLE, raw, w);
	ASSERT_EQ(w[0] == 0x0201 && w[1] == 0x0403 && w[2] == 0x0605 && w[3] == 0x0807);
	ASSERT_EQ(decode_bits(4, WORDS_HIGH_FIRST, BYTES_LITTLE, w) == raw);
	encode_bits(4, WORDS_LOW_FIRST, BYTES_LITTLE, raw, w);
	ASSERT_EQ(w[0] == 0x0807 && w[1] == 0x0605 && w[2] == 0x0403 && w[3] == 0x0201);
	ASSERT_EQ(decode_bits(4, WORDS_LOW_FIRST, BYTES_LITTLE, w) == raw);
}

TEST(Encode64BitIntegersClampAndFloat64KeepsNonFinite)
{
	u16	w[4] = { 0 };

	encode_value(VT_U64, WORDS_HIGH_FIRST, BYTES_BIG, -5, w);
	ASSERT_EQ(decode_bits(4, WORDS_HIGH_FIRST, BYTES_BIG, w) == 0);
	encode_value(VT_U64, WORDS_HIGH_FIRST, BYTES_BIG, 1e30, w);
	ASSERT_EQ(decode_bits(4, WORDS_HIGH_FIRST, BYTES_BIG, w) == UINT64_MAX);
	encode_value(VT_U64, WORDS_HIGH_FIRST, BYTES_BIG, NAN, w);
	ASSERT_EQ(decode_bits(4, WORDS_HIGH_FIRST, BYTES_BIG, w) == 0);
	encode_value(VT_S64, WORDS_HIGH_FIRST, BYTES_BIG, 1e30, w);
	ASSERT_EQ(decode_bits(4, WORDS_HIGH_FIRST, BYTES_BIG, w) == (u64)INT64_MAX);
	encode_value(VT_S64, WORDS_HIGH_FIRST, BYTES_BIG, -1e30, w);
	ASSERT_EQ(decode_bits(4, WORDS_HIGH_FIRST, BYTES_BIG, w) == (u64)INT64_MIN);
	encode_value(VT_F64, WORDS_HIGH_FIRST, BYTES_BIG, -INFINITY, w);
	ASSERT_EQ(isinf(decode_value(VT_F64, WORDS_HIGH_FIRST, BYTES_BIG, w)));
	/* 1.0 = 0x3FF0000000000000 */
	encode_value(VT_F64, WORDS_HIGH_FIRST, BYTES_BIG, 1.0, w);
	ASSERT_EQ(w[0] == 0x3FF0 && w[1] == 0 && w[2] == 0 && w[3] == 0);
}

TEST(DecodeValueHandlesNonFiniteAndWordOrder)
{
	u16	words[4] = { 0x1234, 0x5678 };

	ASSERT_EQ(decode_value(VT_U32, WORDS_HIGH_FIRST, BYTES_BIG, words) == 0x12345678u);
	ASSERT_EQ(decode_value(VT_S32, WORDS_HIGH_FIRST, BYTES_BIG, words) == 0x12345678);
	words[0] = 0x5678;
	words[1] = 0x1234;
	ASSERT_EQ(decode_value(VT_U32, WORDS_LOW_FIRST, BYTES_BIG, words) == 0x12345678u);
	ASSERT_EQ(decode_value(VT_S32, WORDS_LOW_FIRST, BYTES_BIG, words) == 0x12345678);

	words[0] = 0xFFFF;
	words[1] = 0xFFFF;
	ASSERT_EQ(decode_value(VT_U16, WORDS_HIGH_FIRST, BYTES_BIG, words) == 65535);
	ASSERT_EQ(decode_value(VT_S16, WORDS_HIGH_FIRST, BYTES_BIG, words) == -1);
	ASSERT_EQ(decode_value(VT_U32, WORDS_HIGH_FIRST, BYTES_BIG, words) == 4294967295u);
	ASSERT_EQ(decode_value(VT_S32, WORDS_HIGH_FIRST, BYTES_BIG, words) == -1);
	ASSERT_EQ(decode_value(VT_BOOL, WORDS_HIGH_FIRST, BYTES_BIG, words) == 1);
	words[0] = 0;
	ASSERT_EQ(decode_value(VT_BOOL, WORDS_HIGH_FIRST, BYTES_BIG, words) == 0);

	ASSERT_EQ(encode_value(VT_F32, WORDS_HIGH_FIRST, BYTES_BIG, NAN, words) == 2);
	ASSERT_EQ(isnan(decode_value(VT_F32, WORDS_HIGH_FIRST, BYTES_BIG, words)));
	ASSERT_EQ(encode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, INFINITY, words) == 2);
	ASSERT_EQ(isinf(decode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, words)));
	ASSERT_EQ(decode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, words) > 0);
	ASSERT_EQ(encode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, -INFINITY, words) == 2);
	ASSERT_EQ(isinf(decode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, words)));
	ASSERT_EQ(decode_value(VT_F32, WORDS_LOW_FIRST, BYTES_BIG, words) < 0);
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
	ASSERT_FAILURE(device_range_writable(dev, SPACE_HOLDING, 0, 2));
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

static point_t
model_point(space_kind_t space, u32 address, value_type_t type, const model_t* model)
{
	point_t	p = static_point(space, address, type, 0);

	p.kind  = POINT_MODEL;
	p.model = *model;
	return p;
}

static u16
read_word(device_t* dev, space_kind_t space, u32 address)
{
	u16	out = 0;

	ASSERT_SUCCESS(device_read(dev, space, address, 1, &out));
	return out;
}

TEST(SinePointEncodesThroughItsTypeAndFreezesOnWrite)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	model_t		sine  = { .shape = MODEL_SINE, .offset = 100, .amplitude = 50, .period_ms = 4000 };
	point_t		p     = model_point(SPACE_INPUT, 0, VT_S16, &sine);
	u16		w     = 77;

	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 100);
	device_tick(dev, 1000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 150);
	device_tick(dev, 3000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 50);

	/* a point declared as holding is writable; the write freezes the model */
	p = model_point(SPACE_HOLDING, 0, VT_U16, &sine);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, &w));
	device_tick(dev, 1000);
	ASSERT_EQ(read_word(dev, SPACE_HOLDING, 0) == 77);

	device_restart(dev, 10000);
	ASSERT_EQ(read_word(dev, SPACE_HOLDING, 0) == 100);
	device_tick(dev, 11000);
	ASSERT_EQ(read_word(dev, SPACE_HOLDING, 0) == 150);
	arena_release(arena);
}

TEST(LagFollowsAWritableSetpointAndRestartResetsIt)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		sp    = static_point(SPACE_HOLDING, 1, VT_U16, 200);
	model_t		lag   = { .shape = MODEL_LAG, .gain = 1, .tau_ms = 1000, .initial = 200,
				  .has_initial = true, .target_is_point = true,
				  .target_space = SPACE_HOLDING, .target_address = 1 };
	point_t		temp  = model_point(SPACE_INPUT, 0, VT_S16, &lag);
	u8		req[5] = { 0x06, 0, 1, 0x01, 0x2C };	/* write holding 1 = 300 */
	u8		rd[5]  = { 0x04, 0, 0, 0, 1 };
	u8		resp[260];

	ASSERT_EQ(device_add_point(dev, &sp) == NULL);
	ASSERT_EQ(device_add_point(dev, &temp) == NULL);
	device_reset_models(dev, 0);

	ASSERT_EQ(modbus_handle_pdu(dev, 0, rd, 5, resp) == 4);
	ASSERT_EQ(resp[2] == 0 && resp[3] == 200);
	ASSERT_EQ(modbus_handle_pdu(dev, 10000, req, 5, resp) == 5);
	ASSERT_EQ(modbus_handle_pdu(dev, 11000, rd, 5, resp) == 4);
	/* 200 + 100 * (1 - 1/e) = 263.2 after one tau at the new setpoint */
	ASSERT_EQ(((resp[2] << 8) | resp[3]) == 263);
	ASSERT_EQ(modbus_handle_pdu(dev, 60000, rd, 5, resp) == 4);
	ASSERT_EQ(((resp[2] << 8) | resp[3]) == 300);

	device_restart(dev, 70000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 200);
	ASSERT_EQ(read_word(dev, SPACE_HOLDING, 1) == 200);
	arena_release(arena);
}

TEST(HostWriteClaimsAModeledPointAndNoisyStaticIsModeled)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	model_t		noisy = { .shape = MODEL_BASE,
				  .noise = { .amplitude = 5, .every_ms = 100 } };
	point_t		p     = model_point(SPACE_INPUT, 0, VT_U16, &noisy);
	val_t		v     = { .type = VAL_NUM, .n = 42 };

	p.static_value = 1000;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	for (u64 t = 0; t < 5000; t += 100)
	{
		device_tick(dev, t);
		ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) >= 995 &&
			  read_word(dev, SPACE_INPUT, 0) <= 1005);
	}
	ASSERT_SUCCESS(device_set_point(dev, SPACE_INPUT, 0, &v));
	device_tick(dev, 9000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 42);
	ASSERT_EQ(dev->points[0].kind == POINT_MACHINE);
	arena_release(arena);
}

TEST(ModelsAreRejectedOnBitTablesAndWithBadNumbers)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	model_t		sine  = { .shape = MODEL_SINE, .amplitude = 1, .period_ms = 0 };
	model_t		lag   = { .shape = MODEL_LAG, .tau_ms = 0 };
	point_t		p     = model_point(SPACE_INPUT, 0, VT_U16, &sine);

	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	p = model_point(SPACE_INPUT, 0, VT_U16, &lag);
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	sine.period_ms = 1000;
	p = model_point(SPACE_COIL, 0, VT_BOOL, &sine);
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	sine.noise = (struct model_noise_t){ .amplitude = 1, .every_ms = 0 };
	p = model_point(SPACE_INPUT, 0, VT_U16, &sine);
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	arena_release(arena);
}

/* Input 0, holding 0, coil 0 and discrete 0 fail at 5000 ms; the neighbouring points do not. */
static device_t*
make_failing_device(arena_t* arena)
{
	device_t*	dev  = fresh_device(arena);
	model_t		fail = { .degradation = { .has_fail = true, .fail_after_ms = 5000 } };
	point_t		p    = model_point(SPACE_INPUT, 0, VT_U16, &fail);

	p.static_value = 7;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = static_point(SPACE_INPUT, 1, VT_U16, 8);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = model_point(SPACE_HOLDING, 0, VT_U16, &fail);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = static_point(SPACE_HOLDING, 1, VT_U16, 9);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = model_point(SPACE_COIL, 0, VT_BOOL, &fail);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = static_point(SPACE_COIL, 1, VT_BOOL, 1);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = model_point(SPACE_DISCRETE, 0, VT_BOOL, &fail);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	device_reset_models(dev, 0);
	return dev;
}

TEST(DegradationFailAnswersException04OnReadsCoveringThePoint)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_failing_device(arena);
	u8		in0[5]   = { 0x04, 0, 0, 0, 1 };
	u8		in01[5]  = { 0x04, 0, 0, 0, 2 };
	u8		in1[5]   = { 0x04, 0, 1, 0, 1 };
	u8		hold[5]  = { 0x03, 0, 0, 0, 2 };
	u8		hold1[5] = { 0x03, 0, 1, 0, 1 };
	u8		coil[5]  = { 0x01, 0, 0, 0, 2 };
	u8		coil1[5] = { 0x01, 0, 1, 0, 1 };
	u8		disc[5]  = { 0x02, 0, 0, 0, 1 };
	u8		rw[12]   = { 0x17, 0, 0, 0, 1, 0, 1, 0, 1, 2, 0x00, 0x12 };
	u8		rw_ok[12] = { 0x17, 0, 1, 0, 1, 0, 1, 0, 1, 2, 0x00, 0x12 };
	u8		wr[5]    = { 0x06, 0, 0, 0x00, 0x42 };
	u8		resp[260];

	/* before the failure time everything reads normally */
	ASSERT_EQ(modbus_handle_pdu(dev, 4999, in0, 5, resp) == 4);
	ASSERT_EQ(resp[3] == 7);
	ASSERT_EQ(modbus_handle_pdu(dev, 4999, coil, 5, resp) == 3);
	ASSERT_EQ(dev->diag_exception_count == 0);

	ASSERT_EQ(modbus_handle_pdu(dev, 5000, in0, 5, resp) == 2);
	ASSERT_EQ(resp[0] == 0x84 && resp[1] == 0x04);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, in01, 5, resp) == 2 && resp[1] == 0x04);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, hold, 5, resp) == 2);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == 0x04);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, coil, 5, resp) == 2);
	ASSERT_EQ(resp[0] == 0x81 && resp[1] == 0x04);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, disc, 5, resp) == 2);
	ASSERT_EQ(resp[0] == 0x82 && resp[1] == 0x04);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, rw, 12, resp) == 2);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == 0x04);
	ASSERT_EQ(dev->diag_exception_count == 6);
	/* the read-write function does not write when its read fails */
	ASSERT_EQ(read_word(dev, SPACE_HOLDING, 1) == 9);

	/* other addresses still read; an undefined address stays exception 02 */
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, in1, 5, resp) == 4 && resp[3] == 8);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, hold1, 5, resp) == 4 && resp[3] == 9);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, coil1, 5, resp) == 3 && resp[2] == 1);
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, rw_ok, 12, resp) == 4);
	in1[4] = 9;
	ASSERT_EQ(modbus_handle_pdu(dev, 5000, in1, 5, resp) == 2 && resp[1] == 0x02);

	/* writes are not affected, and the written value is still refused on read */
	ASSERT_EQ(modbus_handle_pdu(dev, 6000, wr, 5, resp) == 5);
	ASSERT_EQ(read_word(dev, SPACE_HOLDING, 0) == 0x42);
	ASSERT_EQ(modbus_handle_pdu(dev, 6000, hold, 5, resp) == 2 && resp[1] == 0x04);

	/* a restart heals the device and the timeline starts again */
	device_restart(dev, 10000);
	ASSERT_EQ(modbus_handle_pdu(dev, 14999, in0, 5, resp) == 4 && resp[3] == 7);
	ASSERT_EQ(modbus_handle_pdu(dev, 15000, in0, 5, resp) == 2 && resp[1] == 0x04);
	arena_release(arena);
}

TEST(DegradationFailAppliesToUserDefinedRegistersAndSurvivesAHostClaim)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_failing_device(arena);
	device_custom_fc_t	fc = { .code = 0x41, .case_count = 2 };
	val_t		v = { .type = VAL_NUM, .n = 5 };
	u8		ok[2]  = { 0x41, 0x01 };
	u8		bad[2] = { 0x41, 0x02 };
	u8		in0[5] = { 0x04, 0, 0, 0, 1 };
	u8		resp[260];

	fc.cases[0] = (struct device_custom_case_t){ .prefix = { 0x01 }, .prefix_len = 1,
		.action = CUSTOM_REGISTERS, .space = SPACE_HOLDING, .address = 1, .count = 1 };
	fc.cases[1] = (struct device_custom_case_t){ .prefix = { 0x02 }, .prefix_len = 1,
		.action = CUSTOM_REGISTERS, .space = SPACE_HOLDING, .address = 0, .count = 2 };
	ASSERT_EQ(device_add_custom_fc(dev, &fc) == NULL);

	ASSERT_EQ(modbus_handle_pdu(dev, 9000, ok, 2, resp) == 4);
	ASSERT_EQ(modbus_handle_pdu(dev, 9000, bad, 2, resp) == 2);
	ASSERT_EQ(resp[0] == 0xC1 && resp[1] == 0x04);
	ASSERT_EQ(dev->diag_exception_count == 1);

	/* the host claiming the point removes model effects but not the failure */
	ASSERT_SUCCESS(device_set_point(dev, SPACE_INPUT, 0, &v));
	ASSERT_EQ(modbus_handle_pdu(dev, 9000, in0, 5, resp) == 2 && resp[1] == 0x04);
	device_restart(dev, 20000);
	ASSERT_EQ(modbus_handle_pdu(dev, 20000, in0, 5, resp) == 4 && resp[3] == 7);
	arena_release(arena);
}

TEST(DegradationDriftAndStuckMoveAndFreezeAPoint)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	model_t		m     = { .shape = MODEL_SINE, .offset = 1000, .amplitude = 100,
			      .period_ms = 4000,
			      .degradation = { .after_ms = 1000, .drift_per_hour = 3600,
					       .has_stuck = true, .stuck_after_ms = 6000 } };
	point_t		p     = model_point(SPACE_INPUT, 0, VT_U16, &m);

	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	device_reset_models(dev, 0);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 1000);
	device_tick(dev, 3000);
	/* sine at 3000 ms is 900; drift 3600/h over 2000 ms is +2 */
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 902);
	device_tick(dev, 6000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 1005);
	device_tick(dev, 7000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 1005);
	device_restart(dev, 100000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 1000);
	device_tick(dev, 103000);
	ASSERT_EQ(read_word(dev, SPACE_INPUT, 0) == 902);
	arena_release(arena);
}

TEST(DegradationIsRejectedForDriftOnBitsAndBadNumbers)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	model_t		m     = { .degradation = { .drift_per_hour = 1 } };
	point_t		p     = model_point(SPACE_COIL, 0, VT_BOOL, &m);

	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	m.degradation = (struct model_degradation_t){ .noise_growth_per_hour = 1 };
	p = model_point(SPACE_DISCRETE, 0, VT_BOOL, &m);
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	m.degradation = (struct model_degradation_t){ .noise_growth_per_hour = -1 };
	p = model_point(SPACE_INPUT, 0, VT_U16, &m);
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	m.degradation = (struct model_degradation_t){ .drift_per_hour = INFINITY };
	p = model_point(SPACE_INPUT, 0, VT_U16, &m);
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	m.degradation = (struct model_degradation_t){ .has_stuck = true, .stuck_after_ms = 10 };
	p = model_point(SPACE_COIL, 0, VT_BOOL, &m);
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	arena_release(arena);
}

TEST(ExactStaticSixtyFourBitIntegersSurviveRestartAndClientReads)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 0, VT_U64, 0);
	u16		w[4]  = { 0 };

	p.static_raw   = UINT64_MAX - 1;
	p.static_exact = true;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = static_point(SPACE_HOLDING, 4, VT_S64, 0);
	p.order        = WORDS_LOW_FIRST;
	p.static_raw   = (u64)INT64_MAX;
	p.static_exact = true;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 4, w));
	ASSERT_EQ(w[0] == 0xFFFF && w[1] == 0xFFFF && w[2] == 0xFFFF && w[3] == 0xFFFE);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 4, 4, w));
	ASSERT_EQ(w[0] == 0xFFFF && w[1] == 0xFFFF && w[2] == 0xFFFF && w[3] == 0x7FFF);
	w[0] = w[1] = w[2] = 0;
	w[3] = 7;
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 4, w));
	device_restart(dev, 10);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 4, w));
	ASSERT_EQ(w[3] == 0xFFFE);
	arena_release(arena);
}

TEST(StringPointStoresTextPadsAndRestartsToConfiguredText)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 10, VT_STRING, 0);
	u16		w[3]  = { 0 };
	char		out[8];

	p.length = 3;
	p.text   = "ABCDE";
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 10, 3, w));
	ASSERT_EQ(w[0] == 0x4142 && w[1] == 0x4344 && w[2] == 0x4500);
	ASSERT_SUCCESS(device_get_string(dev, SPACE_HOLDING, 10, out, sizeof(out)));
	ASSERT_EQ(strcmp(out, "ABCDE") == 0);
	ASSERT_FAILURE(device_get_string(dev, SPACE_HOLDING, 10, out, 5));
	ASSERT_FAILURE(device_get_string(dev, SPACE_HOLDING, 11, out, sizeof(out)));

	/* a client writes raw registers: "HI" then NULs */
	w[0] = 0x4849;
	w[1] = 0;
	w[2] = 0;
	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 10, 3, w));
	ASSERT_SUCCESS(device_get_string(dev, SPACE_HOLDING, 10, out, sizeof(out)));
	ASSERT_EQ(strcmp(out, "HI") == 0);

	ASSERT_SUCCESS(device_set_string(dev, SPACE_HOLDING, 10, "XYZ"));
	ASSERT_FAILURE(device_set_string(dev, SPACE_HOLDING, 10, "TOO LONG!"));
	ASSERT_SUCCESS(device_get_string(dev, SPACE_HOLDING, 10, out, sizeof(out)));
	ASSERT_EQ(strcmp(out, "XYZ") == 0);

	device_restart(dev, 10);
	ASSERT_SUCCESS(device_get_string(dev, SPACE_HOLDING, 10, out, sizeof(out)));
	ASSERT_EQ(strcmp(out, "ABCDE") == 0);
	arena_release(arena);
}

TEST(StringPointByteOrderPaddingAndRejections)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_INPUT, 0, VT_STRING, 0);
	u16		w[2]  = { 0 };
	char		out[8];
	val_t		val   = { .type = VAL_NUM, .n = 1 };

	p.length     = 2;
	p.text       = "AB";
	p.padding    = ' ';
	p.byte_order = BYTES_LITTLE;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 0, 2, w));
	ASSERT_EQ(w[0] == 0x4241 && w[1] == 0x2020);
	ASSERT_SUCCESS(device_get_string(dev, SPACE_INPUT, 0, out, sizeof(out)));
	ASSERT_EQ(strcmp(out, "AB  ") == 0);
	ASSERT_FAILURE(device_get_point(dev, SPACE_INPUT, 0, &val));
	ASSERT_FAILURE(device_set_point(dev, SPACE_INPUT, 0, &val));

	/* length bounds, text too long, models and coils */
	p = static_point(SPACE_HOLDING, 100, VT_STRING, 0);
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	p.length = 124;
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	p.length = 123;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	p = static_point(SPACE_HOLDING, 300, VT_STRING, 0);
	p.length = 1;
	p.text   = "ABC";
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	p.text = "AB";
	p.kind = POINT_MODEL;
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	p.kind = POINT_STATIC;
	p.space = SPACE_COIL;
	ASSERT_EQ(device_add_point(dev, &p) != NULL);
	arena_release(arena);
}

TEST(SixtyFourBitPointsReadAndWriteAsDecodedValues)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = fresh_device(arena);
	point_t		p     = static_point(SPACE_HOLDING, 0, VT_F64, 2.5);
	val_t		in    = { .type = VAL_NUM, .n = -7.25 };
	val_t		out   = { 0 };
	u16		w[4]  = { 0 };

	p.byte_order = BYTES_LITTLE;
	p.order      = WORDS_LOW_FIRST;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 0, &out));
	ASSERT_EQ(out.n == 2.5);
	ASSERT_SUCCESS(device_set_point(dev, SPACE_HOLDING, 0, &in));
	ASSERT_SUCCESS(device_get_point(dev, SPACE_HOLDING, 0, &out));
	ASSERT_EQ(out.n == -7.25);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 4, w));
	/* -7.25 = 0xC01D000000000000; low word first, bytes swapped in each register */
	ASSERT_EQ(w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0x1DC0);
	arena_release(arena);
}
