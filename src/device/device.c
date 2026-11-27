/**
 * @file
 * @brief Simulated device: data tables, points, reads and writes, file records, FIFOs, identification.
 */
#include "device/device.h"
#include "device/action.h"
#include "device/machine.h"
#include "os/os_time.h"
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
	device_t*	dev = ARENA_PUSH_ARRAY_ALIGNED(arena, device_t, 1, _Alignof(device_t));

	if (!dev)
		return NULL;

	dev->arena = arena;
	dev->machines = NULL;

	dev->points = ARENA_PUSH_ARRAY_ALIGNED(arena, point_t, DEVICE_MAX_POINTS, _Alignof(point_t));
	if (!dev->points)
		return NULL;

	dev->files = ARENA_PUSH_ARRAY_ALIGNED(arena, device_file_t, DEVICE_MAX_FILES, _Alignof(device_file_t));
	if (!dev->files)
		return NULL;

	dev->fifos = ARENA_PUSH_ARRAY_ALIGNED(arena, device_fifo_t, DEVICE_MAX_FIFOS, _Alignof(device_fifo_t));
	if (!dev->fifos)
		return NULL;

	dev->canopen_objects = ARENA_PUSH_ARRAY_ALIGNED(arena, device_canopen_object_t,
						DEVICE_MAX_CANOPEN_OBJECTS, _Alignof(device_canopen_object_t));
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

bool
point_is_timed(const point_t* point)
{
	return point->kind == POINT_RAMP || point->kind == POINT_MODEL || point->kind == POINT_CLOCK;
}

u32
point_width(const point_t* point)
{
	return point->type == VT_STRING ? point->length : value_width(point->type);
}

static void
wall_refresh(device_t* dev)
{
	dev->wall_ms = dev->wall_fixed_ms != 0 ? dev->wall_fixed_ms : os_wall_ms();
}

/** @brief The clock field of `point` at `wall_ms`; 0 if the calendar cannot be read. */
static f64
clock_value(const point_t* point, s64 wall_ms)
{
	s64		secs = wall_ms >= 0 ? wall_ms / 1000 : -((-wall_ms + 999) / 1000);
	os_civil_t	c;

	if (point->clock_field == CLOCK_UNIX)
		return (f64)secs;
	if (!os_civil_from_unix(secs, point->clock_utc, &c))
		return 0;
	switch (point->clock_field)
	{
	case CLOCK_YEAR:	return c.year;
	case CLOCK_MONTH:	return c.month;
	case CLOCK_DAY:		return c.day;
	case CLOCK_WEEKDAY:	return c.weekday;
	case CLOCK_HOUR:	return c.hour;
	case CLOCK_MINUTE:	return c.min;
	case CLOCK_SECOND:	return c.sec;
	default:		return 0;
	}
}

/** @brief Computes the point's current value and stores it in its address(es). */
static void
point_apply(device_t* dev, u32 index, u64 now_ms)
{
	point_t*	point = &dev->points[index];
	f64		value = point->static_value;
	f64		target = 0;
	u16		words[DEVICE_MAX_STRING_REGISTERS] = { 0 };
	u32		width = 0;

	if (point->type == VT_STRING)
	{
		encode_string(point->byte_order, (const u8*)point->text, point->length, words);
		memcpy(&dev->spaces[point->space].value[point->address], words,
		       point->length * sizeof(u16));
		return;
	}

	if (point->kind == POINT_RAMP)
		value = ramp_value(&point->ramp, dev->start_ms, now_ms);

	if (point->kind == POINT_CLOCK)
		value = clock_value(point, dev->wall_ms);
	else if (point_is_timed(point))
	{
		if (model_needs_target(&point->model))
		{
			val_t	t = { 0 };

			/* A missing target is a load-time error; 0 only covers API-built points. */
			if (device_get_point(dev, (space_kind_t)point->model.target_space,
					     point->model.target_address, &t))
				target = t.type == VAL_BOOL ? (t.b ? 1.0 : 0.0) : t.n;
		}
		value = model_value(&point->model, value, target, dev->start_ms, now_ms,
				    ((u64)dev->unit_id << 32) | index);
	}

	if (point->static_exact &&
	    point->kind == POINT_STATIC)
	{
		width = value_width(point->type);
		encode_bits(width, point->order, point->byte_order, point->static_raw, words);
	}
	else
		width = encode_value(point->type, point->order, point->byte_order, value, words);
	for (u32 i = 0; i < width; i++)
		dev->spaces[point->space].value[point->address + i] = words[i];
}

static const char*
validate_point(const device_t* dev, const point_t* point)
{
	u32		width = point_width(point);
	const space_t*	space = &dev->spaces[point->space];
	f64		steps = 0;

	if (space_is_bit(point->space) != (point->type == VT_BOOL))
		return "bit tables hold on/off values, register tables hold numbers";

	if (point->read_only &&
	    point->space != SPACE_COIL &&
	    point->space != SPACE_HOLDING)
		return "only coils and holding registers can be read-only";

	if (point->byte_order != BYTES_BIG &&
	    point->byte_order != BYTES_LITTLE)
		return "invalid byte order";

	if (point->type == VT_STRING)
	{
		if (point->length < 1 ||
		    point->length > DEVICE_MAX_STRING_REGISTERS)
			return "string length must be between 1 and 123 registers";

		if (point->kind != POINT_STATIC ||
		    point->model.noise.amplitude != 0 ||
		    model_has_degradation(&point->model))
			return "strings are static: no ramp, model, noise or degradation";

		if (point->text &&
		    strlen(point->text) > 2u * point->length)
			return "string text is longer than its registers";
	}

	if (point->address + width > ADDRESS_SPACE_SIZE)
		return "point does not fit before address 65535";

	if (dev->point_count >= DEVICE_MAX_POINTS)
		return "too many points on one device (max 8192)";

	for (u32 i = 0; i < width; i++)
	{
		if (space->owner[point->address + i] != 0)
			return "address is already used by another point";
	}

	if (point->kind == POINT_CLOCK)
	{
		if (space_is_bit(point->space) ||
		    point->type == VT_F32 ||
		    point->type == VT_F64 ||
		    point->type == VT_STRING)
			return "clock points need an integer register type (u16, s16, u32, s32, u64 or s64)";

		if ((unsigned)point->clock_field > CLOCK_UNIX)
			return "invalid clock field";

		if (point->clock_field == CLOCK_UNIX &&
		    (point->type == VT_U16 || point->type == VT_S16))
			return "clock unix needs a 32-bit or 64-bit type (u32, s32, u64 or s64)";

		if (point->model.noise.amplitude != 0 ||
		    model_has_degradation(&point->model))
			return "clock points cannot have noise or degradation";
	}

	if (point->kind == POINT_MODEL ||
	    point->model.noise.amplitude != 0 ||
	    model_has_degradation(&point->model))
	{
		const model_t*	m = &point->model;

		if (space_is_bit(point->space) &&
		    (m->shape != MODEL_BASE || m->noise.amplitude != 0))
			return "coils and discrete inputs cannot have a model or noise";

		if (space_is_bit(point->space) &&
		    (m->degradation.drift_per_hour != 0 || m->degradation.noise_growth_per_hour != 0))
			return "coils and discrete inputs cannot have degradation drift or noise growth";

		if (!isfinite(m->degradation.drift_per_hour) ||
		    !isfinite(m->degradation.noise_growth_per_hour) ||
		    m->degradation.noise_growth_per_hour < 0)
			return "degradation drift must be finite and noise growth finite and 0 or more";

		if (!isfinite(m->noise.amplitude) ||
		    m->noise.amplitude < 0)
			return "noise amplitude must be a finite number, 0 or more";

		if (m->noise.amplitude > 0 &&
		    (m->noise.every_ms == 0 || m->noise.every_ms > 86400000))
			return "noise every-ms must be between 1 and 86400000";

		if (m->shape == MODEL_SINE &&
		    (m->period_ms == 0 || m->period_ms > 86400000 ||
		     !isfinite(m->offset) || !isfinite(m->amplitude) || !isfinite(m->phase_deg)))
			return "sine needs finite numbers and a period-ms between 1 and 86400000";

		if (m->shape == MODEL_LAG &&
		    (m->tau_ms == 0 || m->tau_ms > 86400000 ||
		     !isfinite(m->offset) || !isfinite(m->gain) || !isfinite(m->initial) ||
		     !isfinite(m->target_value)))
			return "lag needs finite numbers and a tau-ms between 1 and 86400000";
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
	u32		width = point_width(point);
	u32		index = dev->point_count;
	char*		text = NULL;

	if (error)
		return error;

	if (point->type == VT_STRING)
	{
		size_t	len = point->text ? strlen(point->text) : 0;

		/* Rounded up to 8 so that the next arena struct stays aligned. */
		text = ARENA_PUSH_ARRAY_ALIGNED(dev->arena, char, (2u * point->length + 7) & ~7u, 8);
		if (!text)
			return "out of memory while storing a string";
		memset(text, point->padding, 2u * point->length);
		if (len)
			memcpy(text, point->text, len);
	}

	wall_refresh(dev);
	dev->points[index] = *point;
	dev->points[index].text = text;
	dev->points[index].configured_kind = point->kind;
	dev->point_count++;

	for (u32 i = 0; i < width; i++)
		dev->spaces[point->space].owner[point->address + i] = (u16)(index + 1);

	point_apply(dev, index, dev->start_ms);
	return NULL;
}

void
device_tick(device_t* dev, u64 now_ms)
{
	dev->now_ms = now_ms;
	wall_refresh(dev);
	for (u32 i = 0; i < dev->point_count; i++)
	{
		if (!point_is_timed(&dev->points[i]))
			continue;
		point_apply(dev, i, now_ms);
	}
	device_machines_advance(dev, now_ms);
}

void
device_reset_models(device_t* dev, u64 now_ms)
{
	for (u32 i = 0; i < dev->point_count; i++)
	{
		if (point_is_timed(&dev->points[i]))
			model_reset(&dev->points[i].model);
	}
	for (u32 i = 0; i < dev->point_count; i++)
	{
		if (point_is_timed(&dev->points[i]))
			point_apply(dev, i, now_ms);
	}
}

bool
device_range_failed(const device_t* dev, space_kind_t space, u32 address, u32 count)
{
	const space_t*	table = &dev->spaces[space];
	u64		elapsed = dev->now_ms > dev->start_ms ? dev->now_ms - dev->start_ms : 0;

	for (u32 i = 0; i < count && address + i < ADDRESS_SPACE_SIZE; i++)
	{
		u16			owner = table->owner[address + i];
		const struct model_degradation_t*	deg = NULL;

		if (owner == 0 ||
		    (u32)owner > dev->point_count)
			continue;

		deg = &dev->points[owner - 1].model.degradation;
		if (deg->has_fail &&
		    elapsed >= deg->fail_after_ms)
			return true;
	}
	return false;
}

void
device_restart(device_t* dev, u64 now_ms)
{
	dev->start_ms = now_ms;
	dev->now_ms   = now_ms;
	wall_refresh(dev);
	for (u32 i = 0; i < dev->point_count; i++)
	{
		point_t*	point = &dev->points[i];
		point_kind_t	kind  = point->configured_kind;

		if (point->kind == POINT_MACHINE)
			kind = POINT_MACHINE;
		point->kind = kind;
		point_apply(dev, i, now_ms);
	}
	device_reset_models(dev, now_ms);
	device_machines_restart(dev, now_ms);
	device_actions_restart(dev);
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

/** @brief True when [address, address+qty) is inside the table and fully defined. */
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
device_range_writable(const device_t* dev, space_kind_t space, u32 address, u32 qty)
{
	const space_t*	table = &dev->spaces[space];

	for (u32 i = 0; i < qty && address + i < ADDRESS_SPACE_SIZE; i++)
	{
		u16 owner = table->owner[address + i];

		if (owner != 0 &&
		    (dev->points[owner - 1].read_only ||
		     dev->points[owner - 1].kind == POINT_MACHINE))
			return false;
	}
	return true;
}

bool
device_client_write(device_t* dev, space_kind_t space, u32 address, u32 qty, const u16* in)
{
	return device_range_writable(dev, space, address, qty) &&
	       device_write(dev, space, address, qty, in);
}

bool
device_get_point(const device_t* dev, space_kind_t space, u32 address, val_t* out)
{
	const point_t*	point = NULL;
	u16		words[4] = { 0 };
	u32		index = device_find_point(dev, space, address);

	if (index == DEVICE_MAX_POINTS || !out)
		return false;

	point = &dev->points[index];
	if (point->type == VT_STRING)
		return false;
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
		out->n    = decode_value(point->type, point->order, point->byte_order, words);
		out->b    = false;
	}
	return true;
}

static bool
set_point(device_t* dev, space_kind_t space, u32 address, const val_t* in, bool machine)
{
	const point_t*	point = NULL;
	u16		words[4] = { 0 };
	u32		index = device_find_point(dev, space, address);
	f64		value = 0;

	if (index == DEVICE_MAX_POINTS || !in ||
	    dev->points[index].type == VT_STRING)
		return false;
	if (in->type != (space_is_bit(space) ? VAL_BOOL : VAL_NUM))
		return false;

	point = &dev->points[index];
	value = in->type == VAL_BOOL ? (in->b ? 1.0 : 0.0) : in->n;
	encode_value(point->type, point->order, point->byte_order, value, words);

	for (u32 i = 0; i < value_width(point->type); i++)
		dev->spaces[space].value[address + i] = words[i];

	if (machine)
		dev->points[index].kind = POINT_MACHINE;
	else if (dev->points[index].kind != POINT_MACHINE)
		dev->points[index].kind = POINT_STATIC;
	return true;
}

bool
device_set_point(device_t* dev, space_kind_t space, u32 address, const val_t* in)
{
	return set_point(dev, space, address, in, true);
}

bool
device_set_point_host(device_t* dev, space_kind_t space, u32 address, const val_t* in)
{
	return set_point(dev, space, address, in, false);
}

bool
device_get_string(const device_t* dev, space_kind_t space, u32 address, char* out, size_t size)
{
	const point_t*	point = NULL;
	u8		bytes[2 * DEVICE_MAX_STRING_REGISTERS];
	u32		index = device_find_point(dev, space, address);
	size_t		len = 0;

	if (index == DEVICE_MAX_POINTS ||
	    dev->points[index].type != VT_STRING ||
	    !out || size == 0)
		return false;

	point = &dev->points[index];
	decode_string(point->byte_order, &dev->spaces[space].value[address], point->length, bytes);
	while (len < 2u * point->length &&
	       bytes[len] != 0)
		len++;
	if (len >= size)
		return false;
	memcpy(out, bytes, len);
	out[len] = '\0';
	return true;
}

bool
device_set_string(device_t* dev, space_kind_t space, u32 address, const char* text)
{
	const point_t*	point = NULL;
	u8		bytes[2 * DEVICE_MAX_STRING_REGISTERS] = { 0 };
	u16		words[DEVICE_MAX_STRING_REGISTERS];
	u32		index = device_find_point(dev, space, address);
	size_t		len = text ? strlen(text) : 0;

	if (index == DEVICE_MAX_POINTS ||
	    dev->points[index].type != VT_STRING ||
	    !text ||
	    len > 2u * dev->points[index].length)
		return false;

	point = &dev->points[index];
	memcpy(bytes, text, len);
	encode_string(point->byte_order, bytes, point->length, words);
	memcpy(&dev->spaces[space].value[address], words, point->length * sizeof(u16));
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

const device_custom_fc_t*
device_find_custom_fc(const device_t* dev, u8 code)
{
	for (u32 i = 0; i < dev->custom_fc_count; i++)
	{
		if (dev->custom_fcs[i].code == code)
			return &dev->custom_fcs[i];
	}
	return NULL;
}

const char*
device_add_custom_fc(device_t* dev, const device_custom_fc_t* fc)
{
	if (device_find_custom_fc(dev, fc->code))
		return "function code is already defined on this device";
	if (dev->custom_fc_count >= DEVICE_MAX_CUSTOM_FCS)
		return "too many user-defined function codes on one device (max 8)";
	if (!dev->custom_fcs)
	{
		dev->custom_fcs = ARENA_PUSH_ARRAY_ALIGNED(dev->arena, device_custom_fc_t, DEVICE_MAX_CUSTOM_FCS, _Alignof(device_custom_fc_t));
		if (!dev->custom_fcs)
			return "out of memory while storing a user-defined function code";
	}
	dev->custom_fcs[dev->custom_fc_count++] = *fc;
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

	/* Full capacity, so device_set_fifo() never needs the arena. */
	copy = ARENA_PUSH_ARRAY(dev->arena, u16, DEVICE_MAX_FIFO_VALUES);
	if (!copy)
		return "out of memory while storing FIFO queue values";
	for (u32 i = 0; i < count; i++)
		copy[i] = values[i];

	fifo = &dev->fifos[dev->fifo_count++];
	fifo->address = address;
	fifo->count   = count;
	fifo->values  = copy;
	return NULL;
}

bool
device_set_fifo(device_t* dev, u16 address, u16 count, const u16* values)
{
	device_fifo_t*	fifo = find_fifo(dev, address);

	if (!fifo ||
	    count > DEVICE_MAX_FIFO_VALUES)
		return false;
	for (u32 i = 0; i < count; i++)
		fifo->values[i] = values[i];
	fifo->count = count;
	return true;
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
