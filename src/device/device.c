#include "device/device.h"
#include <math.h>
#include <string.h>

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

	dev->files = ARENA_PUSH_ARRAY(arena, device_file_t, DEVICE_MAX_FILES);
	if (!dev->files)
		return NULL;

	dev->fifos = ARENA_PUSH_ARRAY(arena, device_fifo_t, DEVICE_MAX_FIFOS);
	if (!dev->fifos)
		return NULL;

	dev->arena            = arena;
	dev->unit_id          = 1;
	dev->exception_status = 0;
	dev->server_id        = 0;
	dev->run_indicator    = 0xFF;
	dev->com_status       = 0;
	dev->com_event_count  = 0;
	dev->message_count    = 0;
	dev->com_event_log_count = 0;
	memset(dev->com_events, 0, sizeof(dev->com_events));
	dev->start_ms         = start_ms;
	strcpy(dev->device_id[DEVICE_ID_VENDOR_NAME], "modbus-sim");
	strcpy(dev->device_id[DEVICE_ID_PRODUCT_CODE], "modbus-sim");
	strcpy(dev->device_id[DEVICE_ID_MAJOR_MINOR_REVISION], "1.0");
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
		return "too many points on one device (max 8192)";

	for (u32 i = 0; i < width; i++)
	{
		if (space->owner[point->address + i] != 0)
			return "address is already used by another point";
	}

	if (point->kind != POINT_RAMP)
		return NULL;

	if (!isfinite(point->ramp.from) ||
	    !isfinite(point->ramp.to) ||
	    !isfinite(point->ramp.step))
		return "ramp numbers must be finite";

	if (point->ramp.step <= 0)
		return "ramp step must be greater than 0";

	if (point->ramp.every_ms == 0)
		return "ramp every-ms must be greater than 0";

	/* ramp_value() stores the step count in u64 and bounce doubles it. Leave room for both. */
	steps = fabs(point->ramp.to - point->ramp.from) / point->ramp.step + 1e-9;
	if (!isfinite(steps) || steps >= 0x1p63)
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

static device_file_t*
find_file(const device_t* dev, u16 file_no)
{
	for (u32 i = 0; i < dev->file_count; i++)
	{
		if (dev->files[i].file_no == file_no)
			return &dev->files[i];
	}
	return NULL;
}

const char*
device_add_file_record(device_t* dev, u16 file_no, u16 record, u16 length,
		       const u16* values)
{
	device_file_t*		file = find_file(dev, file_no);
	file_segment_t*		seg  = NULL;
	u16*			copy = NULL;
	u32			end  = (u32)record + length;

	if (file_no < 0x0001)
		return "file number must be between 1 and 65535";

	if (length == 0)
		return "file record length must be at least 1";

	if (length > DEVICE_MAX_RECORD_VALUES)
		return "too many record values";

	if (end - 1 > 0x270F)
		return "file record does not fit before record 9999";

	if (!file)
	{
		if (dev->file_count >= DEVICE_MAX_FILES)
			return "too many files on one device";
		file = &dev->files[dev->file_count++];
		file->file_no = file_no;
	}

	for (u32 i = 0; i < file->segment_count; i++)
	{
		u32	seg_end = (u32)file->segments[i].record + file->segments[i].length;

		if ((u32)record < seg_end && end > file->segments[i].record)
			return "file record range overlaps another record";
	}

	if (file->segment_count >= DEVICE_MAX_FILE_SEGMENTS)
		return "too many records in one file";

	copy = ARENA_PUSH_ARRAY(dev->arena, u16, length);
	if (!copy)
		return "out of memory while storing file records";

	for (u32 i = 0; i < length; i++)
		copy[i] = values[i];

	seg = &file->segments[file->segment_count++];
	seg->record = record;
	seg->length = length;
	seg->values = copy;
	return NULL;
}

static bool
file_records_defined(const device_file_t* file, u16 record, u16 length)
{
	if (!file || length == 0)
		return false;

	for (u32 i = 0; i < length; i++)
	{
		u32	want  = (u32)record + i;
		bool	found = false;

		for (u32 s = 0; s < file->segment_count; s++)
		{
			const file_segment_t*	seg = &file->segments[s];

			if (want < seg->record || want >= (u32)seg->record + seg->length)
				continue;
			found = true;
			break;
		}
		if (!found)
			return false;
	}
	return true;
}

bool
device_can_write_file_records(const device_t* dev, u16 file_no, u16 record, u16 length)
{
	return file_records_defined(find_file(dev, file_no), record, length);
}

bool
device_read_file_records(const device_t* dev, u16 file_no, u16 record, u16 length,
			 u16* out)
{
	const device_file_t*	file = find_file(dev, file_no);

	if (!file_records_defined(file, record, length))
		return false;

	for (u32 i = 0; i < length; i++)
	{
		u32	want = (u32)record + i;

		for (u32 s = 0; s < file->segment_count; s++)
		{
			const file_segment_t*	seg = &file->segments[s];

			if (want < seg->record || want >= (u32)seg->record + seg->length)
				continue;
			out[i] = seg->values[want - seg->record];
			break;
		}
	}
	return true;
}

bool
device_write_file_records(device_t* dev, u16 file_no, u16 record, u16 length,
			  const u16* in)
{
	device_file_t*	file = find_file(dev, file_no);

	if (!file_records_defined(file, record, length))
		return false;

	for (u32 i = 0; i < length; i++)
	{
		u32	want = (u32)record + i;

		for (u32 s = 0; s < file->segment_count; s++)
		{
			file_segment_t*	seg = &file->segments[s];

			if (want < seg->record || want >= (u32)seg->record + seg->length)
				continue;
			seg->values[want - seg->record] = in[i];
			break;
		}
	}
	return true;
}

static device_fifo_t*
find_fifo(const device_t* dev, u16 address)
{
	for (u32 i = 0; i < dev->fifo_count; i++)
	{
		if (dev->fifos[i].address == address)
			return &dev->fifos[i];
	}
	return NULL;
}

const char*
device_add_fifo(device_t* dev, u16 address, u16 count, const u16* values)
{
	device_fifo_t*	fifo = NULL;
	u16*		copy = NULL;

	if (count > DEVICE_MAX_FIFO_VALUES)
		return "FIFO queue can have at most 31 values";

	if (find_fifo(dev, address))
		return "FIFO address is already defined";

	if (dev->fifo_count >= DEVICE_MAX_FIFOS)
		return "too many FIFO queues on one device";

	if (count > 0)
	{
		copy = ARENA_PUSH_ARRAY(dev->arena, u16, count);
		if (!copy)
			return "out of memory while storing FIFO queue values";
		for (u32 i = 0; i < count; i++)
			copy[i] = values[i];
	}

	fifo = &dev->fifos[dev->fifo_count++];
	fifo->address = address;
	fifo->count   = count;
	fifo->values  = copy;
	return NULL;
}

bool
device_read_fifo(const device_t* dev, u16 address, u16* count, u16* out)
{
	const device_fifo_t*	fifo = find_fifo(dev, address);

	if (!fifo)
		return false;

	*count = fifo->count;
	for (u32 i = 0; i < fifo->count; i++)
		out[i] = fifo->values[i];
	return true;
}
