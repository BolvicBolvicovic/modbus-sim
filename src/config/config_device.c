#include "config/config_device.h"
#include "vlib/macros.h"
#include <string.h>

/* Names the user writes, in the same order as the matching enums. */
static const char* const TYPE_NAMES[]  = { "u16", "s16", "u32", "s32", "float32" };
static const char* const ORDER_NAMES[] = { "high-first", "low-first" };
static const char* const MODE_NAMES[]  = { "wrap", "bounce", "hold" };
static const char* const KIND_NAMES[]  = { "delay", "drop" };

/* --- simple device options --- */

static bool
parse_name(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	const char*	name = NULL;

	if (!form_get_string(ctx, item, &name))
		return false;

	if (strlen(name) >= DEVICE_NAME_MAX)
		return form_fail(ctx, "%s: name is too long (max %d characters)",
				 form_text(ctx, item), DEVICE_NAME_MAX - 1);

	strcpy(dev->name, name);
	return true;
}

static bool
parse_unit_id(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64	unit = 0;

	if (!form_get_int(ctx, item, 0, 255, &unit))
		return false;

	dev->unit_id = (u8)unit;
	return true;
}

static bool
parse_exception_status(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64	status = 0;

	if (!form_get_int(ctx, item, 0, 255, &status))
		return false;

	dev->exception_status = (u8)status;
	return true;
}

static bool
parse_server_id(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64	id = 0;

	if (!form_get_int(ctx, item, 0, 255, &id))
		return false;

	dev->server_id = (u8)id;
	return true;
}

static bool
parse_run_indicator(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64	status = 0;

	if (!form_get_int(ctx, item, 0, 255, &status))
		return false;

	dev->run_indicator = (u8)status;
	return true;
}

/* (note "text") is documentation for humans. It is checked and ignored. */
static bool
parse_note(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	const char*	text = NULL;

	(void)dev;
	return form_get_string(ctx, item, &text);
}

static bool
parse_fault(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	static const char* const keys[] = { "kind", "delay-ms", "percent" };
	s7_pointer	options = s7_cdr(item);
	u32		kind    = 0;
	s64		delay   = 0;
	s64		percent = 100;

	if (dev->fault.kind != FAULT_NONE)
		return form_fail(ctx, "device has more than one (fault ...); only one is allowed");

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "fault"))
		return false;

	if (!form_need(ctx, options, "kind", "fault"))
		return false;

	if (!form_opt_choice(ctx, options, "kind", KIND_NAMES, ARRAY_COUNT(KIND_NAMES), &kind))
		return false;

	if (kind == 0 && !form_need(ctx, options, "delay-ms", "fault with kind delay"))
		return false;

	if (!form_opt_int(ctx, options, "delay-ms", 1, 60000, &delay))
		return false;

	if (!form_opt_int(ctx, options, "percent", 0, 100, &percent))
		return false;

	dev->fault.kind     = (kind == 0) ? FAULT_DELAY : FAULT_DROP;
	dev->fault.delay_ms = (u32)delay;
	dev->fault.percent  = (u32)percent;
	return true;
}

/* --- points --- */

static bool
parse_ramp(form_ctx_t* ctx, s7_pointer item, point_t* point)
{
	static const char* const keys[] = { "from", "to", "step", "every-ms", "mode" };
	s7_pointer	options = s7_cdr(item);
	s64		every   = 1000;
	u32		mode    = RAMP_WRAP;

	point->ramp.step = 1.0;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "ramp"))
		return false;

	if (!form_need(ctx, options, "from", "ramp") || !form_need(ctx, options, "to", "ramp"))
		return false;

	if (!form_opt_number(ctx, options, "from", &point->ramp.from))
		return false;

	if (!form_opt_number(ctx, options, "to", &point->ramp.to))
		return false;

	if (!form_opt_number(ctx, options, "step", &point->ramp.step))
		return false;

	if (!form_opt_int(ctx, options, "every-ms", 1, 86400000, &every))
		return false;

	if (!form_opt_choice(ctx, options, "mode", MODE_NAMES, ARRAY_COUNT(MODE_NAMES), &mode))
		return false;

	point->kind           = POINT_RAMP;
	point->ramp.every_ms  = (u32)every;
	point->ramp.mode      = (ramp_mode_t)mode;
	return true;
}

static bool
parse_static(form_ctx_t* ctx, s7_pointer item, bool is_bit, point_t* point)
{
	bool	on = false;

	point->kind = POINT_STATIC;
	if (!is_bit)
		return form_get_number(ctx, item, &point->static_value);

	if (!form_get_bool(ctx, item, &on))
		return false;

	point->static_value = on ? 1.0 : 0.0;
	return true;
}

/* Reads (static ...) or (ramp ...). With neither, the point is a static 0. */
static bool
parse_behaviour(form_ctx_t* ctx, s7_pointer options, bool is_bit, point_t* point)
{
	s7_pointer	fixed = form_find(ctx, options, "static");
	s7_pointer	moving = form_find(ctx, options, "ramp");

	if (!form_is_absent(ctx, fixed) && !form_is_absent(ctx, moving))
		return form_fail(ctx, "a point cannot have both (static ...) and (ramp ...)");

	if (!form_is_absent(ctx, moving) && is_bit)
		return form_fail(ctx, "%s: coils and discrete inputs can only be (static on/off)",
				 form_text(ctx, moving));

	if (!form_is_absent(ctx, moving))
		return parse_ramp(ctx, moving, point);

	if (!form_is_absent(ctx, fixed))
		return parse_static(ctx, fixed, is_bit, point);

	point->kind = POINT_STATIC;
	return true;
}

/* Reads (type ...) and (word-order ...) for register points. */
static bool
parse_encoding(form_ctx_t* ctx, s7_pointer options, bool is_bit, point_t* point)
{
	u32	type  = VT_U16;
	u32	order = WORDS_HIGH_FIRST;

	point->type = is_bit ? VT_BOOL : VT_U16;
	if (is_bit && !form_is_absent(ctx, form_find(ctx, options, "type")))
		return form_fail(ctx, "coils and discrete inputs are always on/off: remove (type ...)");

	if (!form_opt_choice(ctx, options, "type", TYPE_NAMES, ARRAY_COUNT(TYPE_NAMES), &type))
		return false;

	if (!form_opt_choice(ctx, options, "word-order", ORDER_NAMES, ARRAY_COUNT(ORDER_NAMES), &order))
		return false;

	point->type  = is_bit ? VT_BOOL : (value_type_t)type;
	point->order = (word_order_t)order;
	return true;
}

static bool
parse_point(form_ctx_t* ctx, s7_pointer item, device_t* dev, space_kind_t space)
{
	static const char* const keys[] = { "type", "word-order", "static", "ramp", "count", "note" };
	const char*	head    = form_head_name(item);
	bool		is_bit  = space_is_bit(space);
	s7_pointer	options = s7_cddr(item);
	point_t		point   = { 0 };
	s64		address = 0;
	s64		count   = 1;
	u32		width   = 0;

	if (s7_list_length(ctx->sc, item) < 2 || !s7_is_integer(s7_cadr(item)))
		return form_fail(ctx, "%s: expected (%s ADDRESS ...) with a whole-number address",
				 form_text(ctx, item), head);

	address = (s64)s7_integer(s7_cadr(item));
	if (address < 0 || address > 65535)
		return form_fail(ctx, "%s: address must be between 0 and 65535", form_text(ctx, item));

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), head))
		return false;

	point.space = space;
	if (!parse_encoding(ctx, options, is_bit, &point))
		return false;

	if (!parse_behaviour(ctx, options, is_bit, &point))
		return false;

	if (!form_opt_int(ctx, options, "count", 1, 65536, &count))
		return false;

	width = value_width(point.type);
	if ((u64)count > (u64)DEVICE_MAX_POINTS - dev->point_count)
		return form_fail(ctx, "%s: count %lld would exceed the %d-point limit for one device (%u already used)",
				 form_text(ctx, item), (long long)count, DEVICE_MAX_POINTS, dev->point_count);
	if ((u64)address + (u64)count * width > ADDRESS_SPACE_SIZE)
		return form_fail(ctx, "%s: count %lld does not fit before address 65535",
				 form_text(ctx, item), (long long)count);

	for (s64 i = 0; i < count; i++)
	{
		const char*	error = NULL;

		point.address = (u32)address + (u32)i * width;
		error = device_add_point(dev, &point);
		if (error)
			return form_fail(ctx, "%s %u: %s", head, point.address, error);
	}
	return true;
}

static bool
parse_coil(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	return parse_point(ctx, item, dev, SPACE_COIL);
}

static bool
parse_discrete(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	return parse_point(ctx, item, dev, SPACE_DISCRETE);
}

static bool
parse_input(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	return parse_point(ctx, item, dev, SPACE_INPUT);
}

static bool
parse_holding(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	return parse_point(ctx, item, dev, SPACE_HOLDING);
}

static bool
parse_record_values(form_ctx_t* ctx, s7_pointer rec, u16* values, u32* count)
{
	s7_pointer	v = s7_cddr(rec);

	for (; s7_is_pair(v); v = s7_cdr(v))
	{
		s7_pointer	val = s7_car(v);

		if (!s7_is_integer(val))
			return form_fail(ctx, "%s: record values must be whole numbers",
					 form_text(ctx, rec));
		if ((s64)s7_integer(val) < 0 || (s64)s7_integer(val) > 65535)
			return form_fail(ctx, "%s: record values must be between 0 and 65535",
					 form_text(ctx, rec));
		if (*count >= DEVICE_MAX_RECORD_VALUES)
			return form_fail(ctx, "%s: too many record values (max %d)",
					 form_text(ctx, rec), DEVICE_MAX_RECORD_VALUES);
		values[(*count)++] = (u16)s7_integer(val);
	}
	if (!s7_is_null(ctx->sc, v))
		return form_fail(ctx, "%s: malformed record", form_text(ctx, rec));

	if (*count == 0)
		return form_fail(ctx, "%s: record needs at least one value", form_text(ctx, rec));

	return true;
}

static bool
parse_file_record(form_ctx_t* ctx, s7_pointer item, u16 file_no, s7_pointer rec, device_t* dev)
{
	const char*	rhead  = form_head_name(rec);
	s64		record = 0;
	u32		count  = 0;
	u16		values[DEVICE_MAX_RECORD_VALUES];
	const char*	error  = NULL;

	if (!rhead || strcmp(rhead, "record") != 0)
		return form_fail(ctx, "%s: expected (record REC_NO VALUE ...), got %s",
				 form_text(ctx, item), form_text(ctx, rec));

	if (s7_list_length(ctx->sc, rec) < 3 || !s7_is_integer(s7_cadr(rec)))
		return form_fail(ctx, "%s: expected (record REC_NO VALUE ...)", form_text(ctx, rec));

	record = (s64)s7_integer(s7_cadr(rec));
	if (record < 0 || record > 0x270F)
		return form_fail(ctx, "%s: record number must be between 0 and 9999",
				 form_text(ctx, rec));

	if (!parse_record_values(ctx, rec, values, &count))
		return false;

	error = device_add_file_record(dev, file_no, (u16)record, (u16)count, values);
	if (error)
		return form_fail(ctx, "%s: %s", form_text(ctx, rec), error);

	return true;
}

static bool
parse_file(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64		file_no = 0;
	s7_pointer	p       = NULL;

	if (s7_list_length(ctx->sc, item) < 2 || !s7_is_integer(s7_cadr(item)))
		return form_fail(ctx, "%s: expected (file FILE_NO (record ...) ...)", form_text(ctx, item));

	file_no = (s64)s7_integer(s7_cadr(item));
	if (file_no < 0x0001 || file_no > 0xFFFF)
		return form_fail(ctx, "%s: file number must be between 1 and 65535", form_text(ctx, item));

	if (s7_list_length(ctx->sc, item) < 3)
		return form_fail(ctx, "%s: file needs at least one (record ...)", form_text(ctx, item));

	for (p = s7_cddr(item); s7_is_pair(p); p = s7_cdr(p))
	{
		if (!parse_file_record(ctx, item, (u16)file_no, s7_car(p), dev))
			return false;
	}
	if (!s7_is_null(ctx->sc, p))
		return form_fail(ctx, "%s: malformed file form", form_text(ctx, item));

	return true;
}

static bool
parse_fifo(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64		address  = 0;
	s7_pointer	values_f = NULL;
	const char*	vhead    = NULL;
	u16		values[DEVICE_MAX_FIFO_VALUES];
	u32		count    = 0;

	if (s7_list_length(ctx->sc, item) != 3 || !s7_is_integer(s7_cadr(item)))
		return form_fail(ctx, "%s: expected (fifo ADDRESS (values ...))", form_text(ctx, item));

	address = (s64)s7_integer(s7_cadr(item));
	if (address < 0 || address > 65535)
		return form_fail(ctx, "%s: FIFO address must be between 0 and 65535", form_text(ctx, item));

	values_f = s7_caddr(item);
	vhead    = form_head_name(values_f);
	if (!vhead || strcmp(vhead, "values") != 0)
		return form_fail(ctx, "%s: expected (values ...), got %s", form_text(ctx, item),
				 form_text(ctx, values_f));

	for (s7_pointer v = s7_cdr(values_f); s7_is_pair(v); v = s7_cdr(v))
	{
		s7_pointer	val = s7_car(v);

		if (!s7_is_integer(val))
			return form_fail(ctx, "%s: FIFO values must be whole numbers", form_text(ctx, values_f));
		if ((s64)s7_integer(val) < 0 || (s64)s7_integer(val) > 65535)
			return form_fail(ctx, "%s: FIFO values must be between 0 and 65535", form_text(ctx, values_f));
		if (count >= DEVICE_MAX_FIFO_VALUES)
			return form_fail(ctx, "%s: FIFO queue can have at most %d values",
					 form_text(ctx, values_f), DEVICE_MAX_FIFO_VALUES);
		values[count++] = (u16)s7_integer(val);
	}
	if (!s7_is_null(ctx->sc, s7_cdr(values_f)) && !s7_is_pair(s7_cdr(values_f)))
		return form_fail(ctx, "%s: malformed values list", form_text(ctx, values_f));
	/* guard against improper list tail */
	{
		s7_pointer	v = s7_cdr(values_f);

		while (s7_is_pair(v))
			v = s7_cdr(v);
		if (!s7_is_null(ctx->sc, v))
			return form_fail(ctx, "%s: malformed values list", form_text(ctx, values_f));
	}

	{
		const char*	error = device_add_fifo(dev, (u16)address, (u16)count, values);

		if (error)
			return form_fail(ctx, "%s: %s", form_text(ctx, item), error);
	}
	return true;
}

/* --- device --- */

typedef bool (*item_parser_t)(form_ctx_t*, s7_pointer, device_t*);

typedef struct item_entry_t item_entry_t;
struct item_entry_t
{
	const char*	head;
	item_parser_t	parse;
};

static const item_entry_t DEVICE_ITEMS[] = {
	{ "name",             parse_name             },
	{ "unit-id",          parse_unit_id          },
	{ "exception-status", parse_exception_status },
	{ "server-id",        parse_server_id        },
	{ "run-indicator",    parse_run_indicator    },
	{ "fault",            parse_fault            },
	{ "note",             parse_note             },
	{ "coil",             parse_coil             },
	{ "discrete",         parse_discrete         },
	{ "input",            parse_input            },
	{ "holding",          parse_holding          },
	{ "file",             parse_file             },
	{ "fifo",             parse_fifo             },
};

static const item_entry_t*
find_item(const char* head)
{
	for (u32 i = 0; i < ARRAY_COUNT(DEVICE_ITEMS); i++)
	{
		if (strcmp(DEVICE_ITEMS[i].head, head) == 0)
			return &DEVICE_ITEMS[i];
	}
	return NULL;
}

static bool
parse_item(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	const char*		head  = form_head_name(item);
	const item_entry_t*	entry = head ? find_item(head) : NULL;

	if (!head)
		return form_fail(ctx, "device: expected something like (holding 0 ...), got %s",
				 form_text(ctx, item));

	if (!entry)
		return form_fail(ctx, "device: unknown item '%s'. Use name, unit-id, exception-status, server-id, "
				 "run-indicator, fault, note, coil, discrete, input, holding, file or fifo", head);

	return entry->parse(ctx, item, dev);
}

bool
config_parse_device(form_ctx_t* ctx, s7_pointer form, arena_t* arena, u64 start_ms, device_t** out)
{
	device_t*	dev = device_new(arena, start_ms);

	if (!dev)
		return form_fail(ctx, "out of memory while creating a device");

	strcpy(dev->name, "unnamed");
	for (s7_pointer p = s7_cdr(form); s7_is_pair(p); p = s7_cdr(p))
	{
		if (!parse_item(ctx, s7_car(p), dev))
			return false;
	}

	*out = dev;
	return true;
}
