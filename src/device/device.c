#include "device/device.h"
#include <math.h>

bool
space_is_bit(space_kind_t space)
{
	return space == SPACE_COIL || space == SPACE_DISCRETE;
}

const char*
space_name(space_kind_t space)
{
	static const char* const names[SPACE_COUNT] = { "coil", "discrete", "input", "holding" };

	return names[space];
}

device_t*
device_new(arena_t* arena, u64 start_ms)
{
	device_t*	dev = ARENA_PUSH_STRUCT(arena, device_t);

	if (!dev)
		return NULL;

	dev->points = ARENA_PUSH_ARRAY(arena, point_t, DEVICE_MAX_POINTS);
	if (!dev->points)
		return NULL;

	dev->unit_id          = 1;
	dev->exception_status = 0;
	dev->server_id        = 0;
	dev->run_indicator    = 0xFF;
	dev->com_event_count  = 0;
	dev->message_count    = 0;
	dev->start_ms         = start_ms;
	return dev;
}

/* Computes the point's current value and stores it in its address(es). */
static void
point_apply(device_t* dev, const point_t* point, u64 now_ms)
{
	f64	value = point->static_value;
	u16	words[2] = { 0, 0 };
	u32	width = 0;

	if (point->kind == POINT_RAMP)
		value = ramp_value(&point->ramp, dev->start_ms, now_ms);

	width = encode_value(point->type, point->order, value, words);
	for (u32 i = 0; i < width; i++)
		dev->spaces[point->space].value[point->address + i] = words[i];
}

static const char*
validate_point(const device_t* dev, const point_t* point)
{
	u32		width = value_width(point->type);
	const space_t*	space = &dev->spaces[point->space];
	f64		steps = 0;

	if (space_is_bit(point->space) != (point->type == VT_BOOL))
		return "bit tables hold on/off values, register tables hold numbers";

	if (point->address + width > ADDRESS_SPACE_SIZE)
		return "point does not fit before address 65535";

	if (dev->point_count >= DEVICE_MAX_POINTS)
		return "too many points on one device";

	for (u32 i = 0; i < width; i++)
	{
		if (space->owner[point->address + i] != 0)
			return "address is already used by another point";
	}

	if (point->kind != POINT_RAMP)
		return NULL;

	if (!isfinite(point->ramp.from) || !isfinite(point->ramp.to) || !isfinite(point->ramp.step))
		return "ramp numbers must be finite";

	if (point->ramp.step <= 0)
		return "ramp step must be greater than 0";

	if (point->ramp.every_ms == 0)
		return "ramp every-ms must be greater than 0";

	/* ramp_value() stores the step count in u64 and bounce doubles it. Leave room for both. */
	steps = fabs(point->ramp.to - point->ramp.from) / point->ramp.step + 1e-9;
	if (!isfinite(steps) || steps > (f64)(UINT64_MAX / 2))
		return "ramp range is too large for the step";

	return NULL;
}

const char*
device_add_point(device_t* dev, const point_t* point)
{
	const char*	error = validate_point(dev, point);
	u32		width = value_width(point->type);
	u32		index = dev->point_count;

	if (error)
		return error;

	dev->points[index] = *point;
	dev->point_count++;

	for (u32 i = 0; i < width; i++)
		dev->spaces[point->space].owner[point->address + i] = (u16)(index + 1);

	point_apply(dev, point, dev->start_ms);
	return NULL;
}

void
device_tick(device_t* dev, u64 now_ms)
{
	for (u32 i = 0; i < dev->point_count; i++)
	{
		if (dev->points[i].kind != POINT_RAMP)
			continue;
		point_apply(dev, &dev->points[i], now_ms);
	}
}

/* True when [address, address+qty) is inside the table and fully defined. */
static bool
range_is_defined(const space_t* space, u32 address, u32 qty)
{
	if (qty == 0 || address + qty > ADDRESS_SPACE_SIZE)
		return false;

	for (u32 i = 0; i < qty; i++)
	{
		if (space->owner[address + i] == 0)
			return false;
	}
	return true;
}

bool
device_read(const device_t* dev, space_kind_t space, u32 address, u32 qty, u16* out)
{
	const space_t*	table = &dev->spaces[space];

	if (!range_is_defined(table, address, qty))
		return false;

	for (u32 i = 0; i < qty; i++)
		out[i] = table->value[address + i];
	return true;
}

bool
device_write(device_t* dev, space_kind_t space, u32 address, u32 qty, const u16* in)
{
	space_t*	table = &dev->spaces[space];

	if (!range_is_defined(table, address, qty))
		return false;

	for (u32 i = 0; i < qty; i++)
	{
		u16	owner = table->owner[address + i];

		table->value[address + i] = in[i];
		dev->points[owner - 1].kind = POINT_STATIC;
	}
	return true;
}
