#include "vtest.h"
#include "device/device.h"
#include "device/fault.h"
#include "util/rng.h"
#include <string.h>

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

TEST(FaultNoneNeverFires)
{
	rng_t		rng;
	fault_t		fault = { 0 };

	rng_seed(&rng, 1);
	for (u32 i = 0; i < 1000; i++)
	{
		fault_decision_t	d = fault_decide(&fault, &rng);

		ASSERT_EQ(!d.drop && d.delay_ms == 0);
	}
}

TEST(FaultPercentIsRoughlyRespected)
{
	rng_t		rng;
	fault_t		fault = { .kind = FAULT_DROP, .percent = 25 };
	u32		dropped = 0;

	rng_seed(&rng, 99);
	for (u32 i = 0; i < 10000; i++)
		dropped += fault_decide(&fault, &rng).drop;

	ASSERT_EQ(dropped > 2200 && dropped < 2800);
}

TEST(FaultDelayAlwaysAtHundredPercent)
{
	rng_t		rng;
	fault_t		fault = { .kind = FAULT_DELAY, .delay_ms = 250, .percent = 100 };

	rng_seed(&rng, 5);
	for (u32 i = 0; i < 100; i++)
	{
		fault_decision_t	d = fault_decide(&fault, &rng);

		ASSERT_EQ(!d.drop && d.delay_ms == 250);
	}
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
		ASSERT_EQ(device_add_file_record(dev, (u16)i, 0, 1, &value) == NULL);
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
