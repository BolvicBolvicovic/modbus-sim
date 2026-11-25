#include "device/device.h"
#include "device/machine.h"
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

	dev->arena = arena;
	dev->machines = NULL;

	dev->points = ARENA_PUSH_ARRAY(arena, point_t, DEVICE_MAX_POINTS);
	if (!dev->points)
		return NULL;

	dev->files = ARENA_PUSH_ARRAY(arena, device_file_t, DEVICE_MAX_FILES);
	if (!dev->files)
		return NULL;

	dev->fifos = ARENA_PUSH_ARRAY(arena, device_fifo_t, DEVICE_MAX_FIFOS);
	if (!dev->fifos)
		return NULL;

	dev->canopen_objects = ARENA_PUSH_ARRAY(arena, device_canopen_object_t,
						DEVICE_MAX_CANOPEN_OBJECTS);
	if (!dev->canopen_objects)
		return NULL;

	dev->unit_id          = 1;
	dev->exception_status = 0;
	dev->server_id        = 0;
	dev->run_indicator    = 0xFF;
	dev->server_id_additional_data_len = 0;
	dev->com_status             = 0;
	dev->com_event_count        = 0;
	dev->message_count          = 0;
	dev->com_event_log_count    = 0;
	memset(dev->com_events, 0, sizeof(dev->com_events));
	dev->diagnostic_register    = 0;
	dev->diag_comm_error_count  = 0;
	dev->diag_exception_count   = 0;
	dev->diag_no_response_count = 0;
	dev->diag_nak_count         = 0;
	dev->diag_busy_count        = 0;
	dev->diag_overrun_count     = 0;
	dev->listen_only            = false;
	dev->fault_runtime          = (fault_runtime_t){ 0 };
	dev->start_ms               = start_ms;
	dev->now_ms                 = start_ms;
	strcpy(dev->device_id[DEVICE_ID_VENDOR_NAME], "mogbus");
	strcpy(dev->device_id[DEVICE_ID_PRODUCT_CODE], "mogbus");
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
	dev->points[index].configured_kind = point->kind;
	dev->point_count++;

	for (u32 i = 0; i < width; i++)
		dev->spaces[point->space].owner[point->address + i] = (u16)(index + 1);

	point_apply(dev, point, dev->start_ms);
	return NULL;
}

void
device_tick(device_t* dev, u64 now_ms)
{
	dev->now_ms = now_ms;
	for (u32 i = 0; i < dev->point_count; i++)
	{
		if (dev->points[i].kind != POINT_RAMP)
			continue;
		point_apply(dev, &dev->points[i], now_ms);
	}
	device_machines_advance(dev, now_ms);
}

void
device_restart(device_t* dev, u64 now_ms)
{
	dev->start_ms = now_ms;
	dev->now_ms   = now_ms;
	for (u32 i = 0; i < dev->point_count; i++)
	{
		point_t*	point = &dev->points[i];
		point_kind_t	kind  = point->configured_kind;

		if (point->kind == POINT_MACHINE)
			kind = POINT_MACHINE;
		point->kind = kind;
		point_apply(dev, point, now_ms);
	}
	device_machines_restart(dev, now_ms);
	/* Reboot re-arms every fault schedule from the new start time. The restart
	 * kind keeps its own progress so a scheduled restart advances through its
	 * steps and stops instead of re-arming itself forever; an overlay restart
	 * fault follows the machine's fresh activation time for the same reason. */
	{
		fault_schedule_state_t restart_state = dev->fault_runtime.by_kind[FAULT_RESTART];
		fault_set_t effective = device_effective_faults(dev);

		if ((effective.present & FAULT_BIT(FAULT_RESTART)) != 0 &&
		    effective.by_kind[FAULT_RESTART].source == restart_state.source)
			restart_state.origin_ms = effective.by_kind[FAULT_RESTART].origin_ms;
		dev->fault_runtime = (fault_runtime_t){ 0 };
		dev->fault_runtime.by_kind[FAULT_RESTART] = restart_state;
	}
}

u32
device_find_point(const device_t* dev, space_kind_t space, u32 address)
{
	const point_t*	point = NULL;
	u16		owner = 0;

	if (space >= SPACE_COUNT || address >= ADDRESS_SPACE_SIZE)
		return DEVICE_MAX_POINTS;

	owner = dev->spaces[space].owner[address];
	if (owner == 0 || (u32)owner > dev->point_count)
		return DEVICE_MAX_POINTS;

	point = &dev->points[owner - 1];
	if (point->space != space || point->address != address)
		return DEVICE_MAX_POINTS;
	return owner - 1;
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
	space_t* table = &dev->spaces[space];

	if (!range_is_defined(table, address, qty))
		return false;
	for (u32 i = 0; i < qty; i++)
	{
		u16 owner = table->owner[address + i];

		if (dev->points[owner - 1].kind == POINT_MACHINE)
			return false;
	}

	for (u32 i = 0; i < qty; i++)
	{
		u16 owner = table->owner[address + i];

		table->value[address + i] = in[i];
		if (dev->points[owner - 1].kind != POINT_MACHINE)
			dev->points[owner - 1].kind = POINT_STATIC;
	}
	device_machines_on_write(dev, space, address, qty, dev->now_ms);
	return true;
}

bool
device_get_point(const device_t* dev, space_kind_t space, u32 address, val_t* out)
{
	const point_t*	point = NULL;
	u16		words[2] = { 0, 0 };
	u32		index = device_find_point(dev, space, address);

	if (index == DEVICE_MAX_POINTS || !out)
		return false;

	point = &dev->points[index];
	for (u32 i = 0; i < value_width(point->type); i++)
		words[i] = dev->spaces[space].value[address + i];

	if (space_is_bit(space))
	{
		out->type = VAL_BOOL;
		out->b    = words[0] != 0;
		out->n    = 0;
	}
	else
	{
		out->type = VAL_NUM;
		out->n    = decode_value(point->type, point->order, words);
		out->b    = false;
	}
	return true;
}

bool
device_set_point(device_t* dev, space_kind_t space, u32 address, const val_t* in)
{
	const point_t*	point = NULL;
	u16		words[2] = { 0, 0 };
	u32		index = device_find_point(dev, space, address);
	f64		value = 0;

	if (index == DEVICE_MAX_POINTS || !in)
		return false;
	if (in->type != (space_is_bit(space) ? VAL_BOOL : VAL_NUM))
		return false;

	point = &dev->points[index];
	value = in->type == VAL_BOOL ? (in->b ? 1.0 : 0.0) : in->n;
	encode_value(point->type, point->order, value, words);

	for (u32 i = 0; i < value_width(point->type); i++)
		dev->spaces[space].value[address + i] = words[i];

	dev->points[index].kind = POINT_MACHINE;
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

static device_canopen_object_t*
find_canopen_object(const device_t* dev, u8 node_id, u16 index, u8 subindex)
{
	for (u32 i = 0; i < dev->canopen_object_count; i++)
	{
		device_canopen_object_t* obj = &dev->canopen_objects[i];

		if (obj->node_id == node_id && obj->index == index && obj->subindex == subindex)
			return obj;
	}
	return NULL;
}

const char*
device_add_canopen_object(device_t* dev, u8 node_id, u16 index, u8 subindex,
			  u16 length, const u8* data)
{
	device_canopen_object_t* obj = NULL;

	if (node_id < 1 || node_id > 127)
		return "CANopen node id must be between 1 and 127";

	if (length == 0 || length > DEVICE_MAX_CANOPEN_OBJECT_BYTES)
		return "CANopen object length must be between 1 and 238 bytes";

	if (find_canopen_object(dev, node_id, index, subindex))
		return "CANopen object is already configured for this node, index and subindex";

	if (dev->canopen_object_count >= DEVICE_MAX_CANOPEN_OBJECTS)
		return "too many CANopen objects on one device (max 64)";

	obj = &dev->canopen_objects[dev->canopen_object_count++];
	obj->node_id  = node_id;
	obj->index    = index;
	obj->subindex = subindex;
	obj->length   = length;
	memcpy(obj->data, data, length);
	return NULL;
}

const device_canopen_object_t*
device_find_canopen_object(const device_t* dev, u8 node_id, u16 index, u8 subindex)
{
	return find_canopen_object(dev, node_id, index, subindex);
}

bool
device_canopen_read(const device_t* dev, u8 node_id, u16 index, u8 subindex,
		    u16 offset, u16 count, u8* out)
{
	const device_canopen_object_t* obj = device_find_canopen_object(dev, node_id, index, subindex);

	if (!obj || (u32)offset + count > obj->length)
		return false;

	memcpy(out, obj->data + offset, count);
	return true;
}

bool
device_canopen_write(device_t* dev, u8 node_id, u16 index, u8 subindex,
		     u16 offset, u16 count, const u8* in)
{
	device_canopen_object_t* obj = find_canopen_object(dev, node_id, index, subindex);

	if (!obj || (u32)offset + count > obj->length)
		return false;

	memcpy(obj->data + offset, in, count);
	return true;
}
