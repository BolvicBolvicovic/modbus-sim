/**
 * @file
 * @brief Parses one (device ...) form: points, identification, files, FIFOs, faults, machines.
 */
#include "config/config_device.h"
#include "config/config_fault.h"
#include "config/config_machine.h"
#include "vlib/macros.h"
#include <math.h>
#include <string.h>

typedef struct item_entry_t item_entry_t;

/** @brief Names the user writes, in the same order as the matching enums. */
static const char* const TYPE_NAMES[]  = { "u16", "s16", "u32", "s32", "float32", "u64", "s64", "float64",
	"string" };
static const value_type_t TYPE_VALUES[ARRAY_COUNT(TYPE_NAMES)] = { VT_U16, VT_S16, VT_U32, VT_S32, VT_F32,
	VT_U64, VT_S64, VT_F64, VT_STRING };
static const char* const ORDER_NAMES[] = { "high-first", "low-first" };
static const char* const BYTE_NAMES[]  = { "big", "little" };
static const char* const PADDING_NAMES[] = { "nul", "space" };
static const char* const MODE_NAMES[]  = { "wrap", "bounce", "hold" };
static const char* const DIST_NAMES[]  = { "uniform", "gaussian" };
static const char* const SPACE_NAMES[] = { "coil", "discrete", "input", "holding" };

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
parse_device_id_string(form_ctx_t* ctx, s7_pointer item, device_t* dev, u32 id)
{
	static const char* const names[DEVICE_ID_STANDARD_COUNT] = {
		"vendor-name", "product-code", "major-minor-revision",
		"vendor-url", "product-name", "model-name", "user-application-name",
	};
	const char*	value = NULL;

	if (!form_get_string(ctx, item, &value))
		return false;

	if (strlen(value) > DEVICE_ID_OBJECT_MAX)
		return form_fail(ctx, "%s: %s is too long (max %d bytes, "
				 "limited by FC 43 / 0x2B MEI 14 response size)",
				 form_text(ctx, item), names[id], DEVICE_ID_OBJECT_MAX);

	strcpy(dev->device_id[id], value);
	return true;
}

static bool parse_vendor_name(form_ctx_t* ctx, s7_pointer item, device_t* dev) { return parse_device_id_string(ctx, item, dev, DEVICE_ID_VENDOR_NAME); }
static bool parse_product_code(form_ctx_t* ctx, s7_pointer item, device_t* dev) { return parse_device_id_string(ctx, item, dev, DEVICE_ID_PRODUCT_CODE); }
static bool parse_major_minor_revision(form_ctx_t* ctx, s7_pointer item, device_t* dev) { return parse_device_id_string(ctx, item, dev, DEVICE_ID_MAJOR_MINOR_REVISION); }
static bool parse_vendor_url(form_ctx_t* ctx, s7_pointer item, device_t* dev) { return parse_device_id_string(ctx, item, dev, DEVICE_ID_VENDOR_URL); }
static bool parse_product_name(form_ctx_t* ctx, s7_pointer item, device_t* dev) { return parse_device_id_string(ctx, item, dev, DEVICE_ID_PRODUCT_NAME); }
static bool parse_model_name(form_ctx_t* ctx, s7_pointer item, device_t* dev) { return parse_device_id_string(ctx, item, dev, DEVICE_ID_MODEL_NAME); }
static bool parse_user_application_name(form_ctx_t* ctx, s7_pointer item, device_t* dev) { return parse_device_id_string(ctx, item, dev, DEVICE_ID_USER_APPLICATION_NAME); }

/** @brief Repeatable (device-id-object ID "Text"), stored in ascending ID order. */
static bool
parse_device_id_object(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	const char*	value = NULL;
	s64		id    = 0;
	u32		pos   = 0;

	if (s7_list_length(ctx->sc, item) != 3)
		return form_fail(ctx, "%s: expected (device-id-object ID \"Text\")",
				 form_text(ctx, item));
	if (!s7_is_integer(s7_cadr(item)))
		return form_fail(ctx, "%s: device-id-object ID must be a whole number",
				 form_text(ctx, item));

	id = (s64)s7_integer(s7_cadr(item));
	if (id < 0x80 || id > 0xFF)
		return form_fail(ctx, "%s: device-id-object ID must be between 128 and 255",
				 form_text(ctx, item));
	if (!s7_is_string(s7_caddr(item)))
		return form_fail(ctx, "%s: device-id-object text must be in double quotes",
				 form_text(ctx, item));

	value = s7_string(s7_caddr(item));
	if (value[0] == '\0')
		return form_fail(ctx, "%s: device-id-object text must not be empty",
				 form_text(ctx, item));
	if (strlen(value) > DEVICE_ID_OBJECT_MAX)
		return form_fail(ctx, "%s: device-id-object text is too long (max %d bytes, "
				 "limited by FC 43 / 0x2B MEI response size)",
				 form_text(ctx, item), DEVICE_ID_OBJECT_MAX);
	if (dev->extended_id_count >= DEVICE_MAX_EXTENDED_IDS)
		return form_fail(ctx, "%s: too many device-id-object entries (max %d)",
				 form_text(ctx, item), DEVICE_MAX_EXTENDED_IDS);

	while (pos < dev->extended_id_count && dev->extended_id[pos].id < (u8)id)
		pos++;
	if (pos < dev->extended_id_count && dev->extended_id[pos].id == (u8)id)
		return form_fail(ctx, "%s: device-id-object %d is configured more than once",
				 form_text(ctx, item), (int)id);

	if (pos < dev->extended_id_count)
	{
		memmove(dev->extended_id + pos + 1, dev->extended_id + pos,
			(dev->extended_id_count - pos) * sizeof(dev->extended_id[0]));
	}
	dev->extended_id[pos].id = (u8)id;
	strcpy(dev->extended_id[pos].value, value);
	dev->extended_id_count++;
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

static int
hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool
parse_server_id_additional_data(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	u8	bytes[DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX];
	const char*	label = "server-id-additional-data";
	const char*	text  = NULL;
	size_t	len   = 0;

	if (!form_get_string(ctx, item, &text))
		return false;

	len = strlen(text);
	if (len > 2u * DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX)
		return form_fail(ctx, "%s: too many hex digits (max %d bytes of additional data, "
				 "limited by the FC 17 response PDU size)",
				 label, DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX);
	if (len % 2 != 0)
		return form_fail(ctx, "%s: expected an even number of hex digits", label);

	for (size_t i = 0; i < len; i += 2)
	{
		int	hi = hex_digit(text[i]);
		int	lo = hex_digit(text[i + 1]);

		if (hi < 0 || lo < 0)
			return form_fail(ctx, "%s: invalid hex digit at position %u (use 0-9, a-f or A-F)",
					 label, (u32)(hi < 0 ? i : i + 1));
		bytes[i / 2] = (u8)((hi << 4) | lo);
	}

	memcpy(dev->server_id_additional_data, bytes, len / 2);
	dev->server_id_additional_data_len = (u8)(len / 2);
	return true;
}

/** @brief Repeatable (canopen-object NODE INDEX SUBINDEX "HEXBYTES"). */
static bool
parse_canopen_object(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s7_pointer		hex_f = NULL;
	const char*		text  = NULL;
	const char*		error = NULL;
	s64			node  = 0;
	s64			index = 0;
	s64			sub   = 0;
	u8			bytes[DEVICE_MAX_CANOPEN_OBJECT_BYTES];
	size_t			len   = 0;

	if (s7_list_length(ctx->sc, item) != 5 ||
	    !s7_is_integer(s7_list_ref(ctx->sc, item, 1)) ||
	    !s7_is_integer(s7_list_ref(ctx->sc, item, 2)) ||
	    !s7_is_integer(s7_list_ref(ctx->sc, item, 3)))
		return form_fail(ctx, "%s: expected (canopen-object NODE INDEX SUBINDEX \"HEXBYTES\")",
				 form_text(ctx, item));

	node  = s7_integer(s7_list_ref(ctx->sc, item, 1));
	index = s7_integer(s7_list_ref(ctx->sc, item, 2));
	sub   = s7_integer(s7_list_ref(ctx->sc, item, 3));
	if (node < 1 || node > 127)
		return form_fail(ctx, "%s: CANopen node must be between 1 and 127", form_text(ctx, item));
	if (index < 0 || index > 65535)
		return form_fail(ctx, "%s: CANopen index must be between 0 and 65535", form_text(ctx, item));
	if (sub < 0 || sub > 255)
		return form_fail(ctx, "%s: CANopen subindex must be between 0 and 255", form_text(ctx, item));

	hex_f = s7_list_ref(ctx->sc, item, 4);
	if (!s7_is_string(hex_f))
		return form_fail(ctx, "%s: HEXBYTES must be in double quotes", form_text(ctx, item));

	text = s7_string(hex_f);
	len  = strlen(text);
	if (len < 2)
		return form_fail(ctx, "%s: too few hex digits (at least 1 byte)", form_text(ctx, item));
	if (len > 2u * DEVICE_MAX_CANOPEN_OBJECT_BYTES)
		return form_fail(ctx, "%s: too many hex digits (max %u bytes of CANopen object data)",
				 form_text(ctx, item), (u32)DEVICE_MAX_CANOPEN_OBJECT_BYTES);
	if (len % 2 != 0)
		return form_fail(ctx, "%s: expected an even number of hex digits", form_text(ctx, item));

	for (size_t i = 0; i < len; i += 2)
	{
		int	hi = hex_digit(text[i]);
		int	lo = hex_digit(text[i + 1]);

		if (hi < 0 || lo < 0)
			return form_fail(ctx, "%s: invalid hex digit at position %u (use 0-9, a-f or A-F)",
					 form_text(ctx, item), (u32)(hi < 0 ? i : i + 1));
		bytes[i / 2] = (u8)((hi << 4) | lo);
	}

	error = device_add_canopen_object(dev, (u8)node, (u16)index, (u8)sub, (u16)(len / 2), bytes);
	if (error)
		return form_fail(ctx, "%s: %s", form_text(ctx, item), error);
	return true;
}

static bool
parse_com_status(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64	status = 0;

	if (!form_get_int(ctx, item, 0, 65535, &status))
		return false;
	if (status != 0 && status != 65535)
		return form_fail(ctx, "%s: must be 0 (ready) or 65535 (busy)",
				 form_text(ctx, item));

	dev->com_status = (u16)status;
	return true;
}

static bool
parse_com_event(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	s64	event = 0;

	if (!form_get_int(ctx, item, 0, 255, &event))
		return false;
	if (dev->com_event_log_count >= DEVICE_MAX_COM_EVENTS)
		return form_fail(ctx, "%s: too many (com-event ...) entries (max %d)",
				 form_text(ctx, item), DEVICE_MAX_COM_EVENTS);

	dev->com_events[dev->com_event_log_count++] = (u8)event;
	return true;
}

/**
 * @brief (note "text") is documentation for humans.
 *
 * It is checked and ignored.
 */
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
	return config_parse_fault(ctx, item, &dev->faults, "device");
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
	if (point->type == VT_STRING)
	{
		const char*	text = NULL;

		if (!form_get_string(ctx, item, &text))
			return false;
		for (const char* c = text; *c; c++)
		{
			if (*c < 0x20 ||
			    *c > 0x7E)
				return form_fail(ctx, "%s: string text can only use printable ASCII (0x20-0x7E)",
						 form_text(ctx, item));
		}
		if (strlen(text) > 2u * point->length)
			return form_fail(ctx, "%s: text is longer than %u bytes (2 per register, length %u)",
					 form_text(ctx, item), 2u * point->length, (u32)point->length);
		point->text = text;
		return true;
	}

	if (!is_bit)
	{
		if (!form_get_number(ctx, item, &point->static_value))
			return false;
		if (!isfinite(point->static_value) &&
		    point->type != VT_F32 &&
		    point->type != VT_F64)
			return form_fail(ctx, "%s: only float32 and float64 static values can be NaN or infinity",
					 form_text(ctx, item));

		/* s7 integers are 64-bit: keep a 64-bit integer exactly instead of through f64. */
		if ((point->type == VT_U64 || point->type == VT_S64) &&
		    s7_is_integer(s7_cadr(item)))
		{
			point->static_raw   = (u64)s7_integer(s7_cadr(item));
			point->static_exact = true;
			point->static_value = point->type == VT_U64 ? (f64)point->static_raw :
					      (f64)(s64)point->static_raw;
		}
		return true;
	}

	if (!form_get_bool(ctx, item, &on))
		return false;

	point->static_value = on ? 1.0 : 0.0;
	return true;
}

static bool
parse_sine(form_ctx_t* ctx, s7_pointer item, point_t* point)
{
	static const char* const keys[] = { "offset", "amplitude", "period-ms", "phase-deg" };
	s7_pointer	options = s7_cdr(item);
	s64		period  = 0;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "sine") ||
	    !form_need(ctx, options, "offset", "sine") ||
	    !form_need(ctx, options, "amplitude", "sine") ||
	    !form_need(ctx, options, "period-ms", "sine"))
		return false;

	if (!form_opt_number(ctx, options, "offset", &point->model.offset) ||
	    !form_opt_number(ctx, options, "amplitude", &point->model.amplitude) ||
	    !form_opt_number(ctx, options, "phase-deg", &point->model.phase_deg) ||
	    !form_opt_int(ctx, options, "period-ms", 1, 86400000, &period))
		return false;

	if (!isfinite(point->model.offset) ||
	    !isfinite(point->model.amplitude) ||
	    !isfinite(point->model.phase_deg))
		return form_fail(ctx, "%s: sine numbers must be finite", form_text(ctx, item));

	point->kind              = POINT_MODEL;
	point->model.shape       = MODEL_SINE;
	point->model.period_ms   = (u32)period;
	return true;
}

/** @brief Reads (target NUMBER) or (target (holding|input|coil|discrete ADDRESS)). */
static bool
parse_lag_target(form_ctx_t* ctx, s7_pointer options, model_t* model)
{
	s7_pointer	form  = form_find(ctx, options, "target");
	s7_pointer	value = NULL;
	const char*	head  = NULL;
	s64		address = 0;

	if (!form_need(ctx, options, "target", "lag"))
		return false;

	if (s7_list_length(ctx->sc, form) != 2)
		return form_fail(ctx, "%s: expected (target NUMBER) or (target (holding ADDRESS))",
				 form_text(ctx, form));

	value = s7_cadr(form);
	if (s7_is_real(value))
	{
		model->target_value = (f64)s7_real(value);
		if (!isfinite(model->target_value))
			return form_fail(ctx, "%s: lag numbers must be finite", form_text(ctx, form));
		return true;
	}

	head = form_head_name(value);
	if (!head ||
	    s7_list_length(ctx->sc, value) != 2 ||
	    !s7_is_integer(s7_cadr(value)))
		return form_fail(ctx, "%s: expected a number or a point like (holding 1)",
				 form_text(ctx, form));

	for (u32 space = 0; space < ARRAY_COUNT(SPACE_NAMES); space++)
	{
		if (strcmp(head, SPACE_NAMES[space]) != 0)
			continue;

		address = (s64)s7_integer(s7_cadr(value));
		if (address < 0 || address > 65535)
			return form_fail(ctx, "%s: address must be between 0 and 65535",
					 form_text(ctx, value));
		model->target_is_point = true;
		model->target_space    = space;
		model->target_address  = (u32)address;
		return true;
	}
	return form_fail(ctx, "%s: expected a number or a point like (holding 1)",
			 form_text(ctx, form));
}

static bool
parse_lag(form_ctx_t* ctx, s7_pointer item, point_t* point)
{
	static const char* const keys[] = { "target", "tau-ms", "initial", "gain", "offset" };
	s7_pointer	options = s7_cdr(item);
	model_t*	model   = &point->model;
	s64		tau     = 0;

	model->gain = 1.0;
	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "lag") ||
	    !form_need(ctx, options, "tau-ms", "lag") ||
	    !parse_lag_target(ctx, options, model))
		return false;

	model->has_initial = !form_is_absent(ctx, form_find(ctx, options, "initial"));
	if (!form_opt_int(ctx, options, "tau-ms", 1, 86400000, &tau) ||
	    !form_opt_number(ctx, options, "initial", &model->initial) ||
	    !form_opt_number(ctx, options, "gain", &model->gain) ||
	    !form_opt_number(ctx, options, "offset", &model->offset))
		return false;

	if (!isfinite(model->initial) ||
	    !isfinite(model->gain) ||
	    !isfinite(model->offset))
		return form_fail(ctx, "%s: lag numbers must be finite", form_text(ctx, item));

	point->kind      = POINT_MODEL;
	model->shape     = MODEL_LAG;
	model->tau_ms    = (u32)tau;
	return true;
}

static bool
parse_noise(form_ctx_t* ctx, s7_pointer item, point_t* point)
{
	static const char* const keys[] = { "amplitude", "every-ms", "distribution" };
	s7_pointer	options = s7_cdr(item);
	s64		every   = 1000;
	u32		dist    = 0;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "noise") ||
	    !form_need(ctx, options, "amplitude", "noise"))
		return false;

	if (!form_opt_number(ctx, options, "amplitude", &point->model.noise.amplitude) ||
	    !form_opt_int(ctx, options, "every-ms", 1, 86400000, &every) ||
	    !form_opt_choice(ctx, options, "distribution", DIST_NAMES, ARRAY_COUNT(DIST_NAMES), &dist))
		return false;

	if (!isfinite(point->model.noise.amplitude) ||
	    point->model.noise.amplitude < 0)
		return form_fail(ctx, "%s: noise amplitude must be 0 or more", form_text(ctx, item));

	point->model.noise.every_ms  = (u32)every;
	point->model.noise.gaussian  = dist == 1;
	return true;
}

static bool
parse_degradation(form_ctx_t* ctx, s7_pointer item, bool is_bit, point_t* point)
{
	static const char* const keys[] = { "after-ms", "drift-per-hour", "noise-growth-per-hour",
		"stuck-after-ms", "fail-after-ms" };
	s7_pointer	options = s7_cdr(item);
	struct model_degradation_t*	deg = &point->model.degradation;
	s64		after = 0;
	s64		stuck = 0;
	s64		fail  = 0;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "degradation"))
		return false;

	if (!form_opt_int(ctx, options, "after-ms", 0, 4294967295LL, &after) ||
	    !form_opt_int(ctx, options, "stuck-after-ms", 0, 4294967295LL, &stuck) ||
	    !form_opt_int(ctx, options, "fail-after-ms", 0, 4294967295LL, &fail) ||
	    !form_opt_number(ctx, options, "drift-per-hour", &deg->drift_per_hour) ||
	    !form_opt_number(ctx, options, "noise-growth-per-hour", &deg->noise_growth_per_hour))
		return false;

	if (!isfinite(deg->drift_per_hour) ||
	    !isfinite(deg->noise_growth_per_hour))
		return form_fail(ctx, "%s: degradation numbers must be finite", form_text(ctx, item));

	if (deg->noise_growth_per_hour < 0)
		return form_fail(ctx, "%s: noise-growth-per-hour must be 0 or more", form_text(ctx, item));

	deg->after_ms        = (u32)after;
	deg->has_stuck       = !form_is_absent(ctx, form_find(ctx, options, "stuck-after-ms"));
	deg->stuck_after_ms  = (u32)stuck;
	deg->has_fail        = !form_is_absent(ctx, form_find(ctx, options, "fail-after-ms"));
	deg->fail_after_ms   = (u32)fail;

	if (!model_has_degradation(&point->model))
		return form_fail(ctx, "%s: degradation needs at least one of drift-per-hour, "
				      "noise-growth-per-hour, stuck-after-ms and fail-after-ms",
				 form_text(ctx, item));

	if (is_bit &&
	    (deg->drift_per_hour != 0 ||
	     deg->noise_growth_per_hour != 0))
		return form_fail(ctx, "%s: coils and discrete inputs only support stuck-after-ms and "
				      "fail-after-ms", form_text(ctx, item));
	return true;
}

/**
 * @brief Reads (static ...), (ramp ...), (sine ...) or (lag ...), then an optional (noise ...)
 *        and (degradation ...).
 *
 * With none of the four, the point is a static 0.
 */
static bool
parse_behaviour(form_ctx_t* ctx, s7_pointer options, bool is_bit, point_t* point)
{
	static const char* const names[] = { "static", "ramp", "sine", "lag" };
	s7_pointer	found[ARRAY_COUNT(names)];
	s7_pointer	noise = form_find(ctx, options, "noise");
	s7_pointer	degradation = form_find(ctx, options, "degradation");
	u32		present = 0;

	for (u32 i = 0; i < ARRAY_COUNT(names); i++)
	{
		found[i] = form_find(ctx, options, names[i]);
		if (!form_is_absent(ctx, found[i]))
			present++;
	}

	if (present > 1)
		return form_fail(ctx, "a point can have only one of (static ...), (ramp ...), (sine ...) and (lag ...)");

	if (point->type == VT_STRING)
	{
		for (u32 i = 1; i < ARRAY_COUNT(names); i++)
		{
			if (!form_is_absent(ctx, found[i]))
				return form_fail(ctx, "%s: string points can only be (static \"TEXT\")",
						 form_text(ctx, found[i]));
		}
		if (!form_is_absent(ctx, noise) ||
		    !form_is_absent(ctx, degradation))
			return form_fail(ctx, "%s: string points cannot have (noise ...) or (degradation ...)",
					 form_text(ctx, form_is_absent(ctx, noise) ? degradation : noise));
	}

	if (is_bit)
	{
		for (u32 i = 1; i < ARRAY_COUNT(names); i++)
		{
			if (!form_is_absent(ctx, found[i]))
				return form_fail(ctx, "%s: coils and discrete inputs can only be (static on/off)",
						 form_text(ctx, found[i]));
		}
		if (!form_is_absent(ctx, noise))
			return form_fail(ctx, "%s: coils and discrete inputs cannot have (noise ...)",
					 form_text(ctx, noise));
	}

	if (!form_is_absent(ctx, noise) && present == 0)
		return form_fail(ctx, "%s: (noise ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to disturb",
				 form_text(ctx, noise));

	if (!form_is_absent(ctx, degradation) && present == 0)
		return form_fail(ctx, "%s: (degradation ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to age",
				 form_text(ctx, degradation));

	point->kind = POINT_STATIC;
	if (!form_is_absent(ctx, found[1]) && !parse_ramp(ctx, found[1], point))
		return false;
	if (!form_is_absent(ctx, found[2]) && !parse_sine(ctx, found[2], point))
		return false;
	if (!form_is_absent(ctx, found[3]) && !parse_lag(ctx, found[3], point))
		return false;
	if (!form_is_absent(ctx, found[0]) && !parse_static(ctx, found[0], is_bit, point))
		return false;

	if (!form_is_absent(ctx, noise) && !parse_noise(ctx, noise, point))
		return false;

	if (!form_is_absent(ctx, degradation) && !parse_degradation(ctx, degradation, is_bit, point))
		return false;

	/* A static value with noise or degradation is evaluated like a model; a ramp keeps its own kind. */
	if (point->kind == POINT_STATIC &&
	    (point->model.noise.amplitude > 0 ||
	     model_has_degradation(&point->model)))
		point->kind = POINT_MODEL;
	return true;
}

/** @brief Reads (type ...), (word-order ...), (byte-order ...), (length ...) and (padding ...). */
static bool
parse_encoding(form_ctx_t* ctx, s7_pointer options, bool is_bit, point_t* point)
{
	u32	type  = 0;
	u32	order = WORDS_HIGH_FIRST;
	u32	bytes = BYTES_BIG;
	u32	padding = 0;
	s64	length  = 0;

	point->type = is_bit ? VT_BOOL : VT_U16;
	if (is_bit && !form_is_absent(ctx, form_find(ctx, options, "type")))
		return form_fail(ctx, "coils and discrete inputs are always on/off: remove (type ...)");

	if (!form_opt_choice(ctx, options, "type", TYPE_NAMES, ARRAY_COUNT(TYPE_NAMES), &type))
		return false;

	if (!form_opt_choice(ctx, options, "word-order", ORDER_NAMES, ARRAY_COUNT(ORDER_NAMES), &order))
		return false;

	if (!form_opt_choice(ctx, options, "byte-order", BYTE_NAMES, ARRAY_COUNT(BYTE_NAMES), &bytes))
		return false;

	if (is_bit &&
	    !form_is_absent(ctx, form_find(ctx, options, "byte-order")))
		return form_fail(ctx, "coils and discrete inputs are single bits: remove (byte-order ...)");

	point->type       = is_bit ? VT_BOOL : TYPE_VALUES[type];
	point->order      = (word_order_t)order;
	point->byte_order = (byte_order_t)bytes;

	if (point->type != VT_STRING)
	{
		if (!form_is_absent(ctx, form_find(ctx, options, "length")) ||
		    !form_is_absent(ctx, form_find(ctx, options, "padding")))
			return form_fail(ctx, "(length ...) and (padding ...) only apply to (type string)");
		return true;
	}

	if (!form_is_absent(ctx, form_find(ctx, options, "word-order")))
		return form_fail(ctx, "strings have no word order: remove (word-order ...)");
	if (!form_need(ctx, options, "length", "a string point") ||
	    !form_opt_int(ctx, options, "length", 1, DEVICE_MAX_STRING_REGISTERS, &length) ||
	    !form_opt_choice(ctx, options, "padding", PADDING_NAMES, ARRAY_COUNT(PADDING_NAMES), &padding))
		return false;
	point->length  = (u16)length;
	point->padding = padding == 1 ? ' ' : '\0';
	return true;
}

static bool
parse_point(form_ctx_t* ctx, s7_pointer item, device_t* dev, space_kind_t space)
{
	static const char* const keys[] = { "type", "word-order", "byte-order", "length", "padding", "static", "ramp", "sine", "lag",
		"noise", "degradation", "count", "note" };
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

	width = point_width(&point);
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
	s7_pointer	v        = NULL;

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

	for (v = s7_cdr(values_f); s7_is_pair(v); v = s7_cdr(v))
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
	if (!s7_is_null(ctx->sc, v))
		return form_fail(ctx, "%s: malformed values list", form_text(ctx, values_f));

	{
		const char*	error = device_add_fifo(dev, (u16)address, (u16)count, values);

		if (error)
			return form_fail(ctx, "%s: %s", form_text(ctx, item), error);
	}
	return true;
}

/* Function codes the PDU handler implements; a device may define only the others. Kept equal to
 * modbus_fc_is_builtin() by a test (config cannot include modbus/). */
static const u8 BUILTIN_FCS[] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0B, 0x0C, 0x0F, 0x10, 0x11,
	0x14, 0x15, 0x16, 0x17, 0x18, 0x2B,
};

bool
config_fc_is_builtin(u8 fc)
{
	for (u32 i = 0; i < ARRAY_COUNT(BUILTIN_FCS); i++)
	{
		if (BUILTIN_FCS[i] == fc)
			return true;
	}
	return false;
}

/* "01 0A ff" -> bytes: pairs of hex digits, spaces between pairs allowed. */
static bool
parse_hex_bytes(form_ctx_t* ctx, s7_pointer value, const char* what, u8* out, u32 cap, u32* len)
{
	const char*	text;
	u32		n = 0;

	if (!s7_is_string(value))
		return form_fail(ctx, "%s: expected a string of hex bytes like \"01 0A FF\", got %s", what,
				 form_text(ctx, value));
	text = s7_string(value);
	for (u32 i = 0; text[i];)
	{
		int hi;
		int lo;

		if (text[i] == ' ')
		{
			i++;
			continue;
		}
		hi = hex_digit(text[i]);
		lo = text[i + 1] ? hex_digit(text[i + 1]) : -1;
		if (hi < 0 || lo < 0)
			return form_fail(ctx, "%s: \"%s\" is not a list of hex bytes (two digits 0-9, a-f or A-F "
					 "per byte, spaces between bytes)", what, text);
		if (n >= cap)
			return form_fail(ctx, "%s: too many bytes (max %u)", what, cap);
		out[n++] = (u8)((hi << 4) | lo);
		i += 2;
	}
	*len = n;
	return true;
}

/* One action of a user-defined function code: (reply "HEX"), (echo), (exception N),
 * (registers SPACE ADDRESS COUNT) or (no-response). */
static bool
parse_custom_action(form_ctx_t* ctx, s7_pointer action, struct device_custom_case_t* c)
{
	static const char* const SPACES[] = { "coil", "discrete", "input", "holding" };
	const char*	head = form_head_name(action);
	long		len  = s7_list_length(ctx->sc, action);
	u32		n    = 0;

	if (!head)
		return form_fail(ctx, "%s: expected (reply \"HEX\"), (echo), (exception N), "
				 "(registers SPACE ADDRESS COUNT) or (no-response)", form_text(ctx, action));
	if (strcmp(head, "reply") == 0 && len == 2)
	{
		c->action = CUSTOM_REPLY;
		if (!parse_hex_bytes(ctx, s7_cadr(action), "reply", c->reply, DEVICE_CUSTOM_REPLY_MAX, &n))
			return false;
		c->reply_len = (u16)n;
		return true;
	}
	if (strcmp(head, "echo") == 0 && len == 1)
	{
		c->action = CUSTOM_ECHO;
		return true;
	}
	if (strcmp(head, "no-response") == 0 && len == 1)
	{
		c->action = CUSTOM_NO_RESPONSE;
		return true;
	}
	if (strcmp(head, "exception") == 0 && len == 2)
	{
		s64 code = 0;

		c->action = CUSTOM_EXCEPTION;
		if (!form_get_int(ctx, action, 1, 255, &code))
			return false;
		c->exception_code = (u8)code;
		return true;
	}
	if (strcmp(head, "registers") == 0 && len == 4)
	{
		const char*	space = s7_is_symbol(s7_cadr(action)) ? s7_symbol_name(s7_cadr(action)) : "";
		s7_pointer	addr  = s7_caddr(action);
		s7_pointer	count = s7_cadddr(action);
		u32		max;

		c->action = CUSTOM_REGISTERS;
		c->space  = SPACE_COUNT;
		for (u32 i = 0; i < ARRAY_COUNT(SPACES); i++)
		{
			if (strcmp(space, SPACES[i]) == 0)
				c->space = (space_kind_t)i;
		}
		if (c->space == SPACE_COUNT)
			return form_fail(ctx, "%s: the space must be coil, discrete, input or holding",
					 form_text(ctx, action));
		max = space_is_bit(c->space) ? 2000 : 125;
		if (!s7_is_integer(addr) ||
		    !s7_is_integer(count) ||
		    s7_integer(addr) < 0 ||
		    s7_integer(count) < 1 ||
		    (u64)s7_integer(count) > max ||
		    s7_integer(addr) + s7_integer(count) > 65536)
			return form_fail(ctx, "%s: expected ADDRESS 0-65535 and COUNT 1-%u that stay below 65536",
					 form_text(ctx, action), max);
		c->address = (u16)s7_integer(addr);
		c->count   = (u16)s7_integer(count);
		return true;
	}
	return form_fail(ctx, "%s: expected (reply \"HEX\"), (echo), (exception N), "
			 "(registers SPACE ADDRESS COUNT) or (no-response)", form_text(ctx, action));
}

/* Repeatable (function-code CODE [(name "TEXT")] (on "HEX" ACTION)... | ACTION). */
static bool
parse_function_code(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	device_custom_fc_t	fc = { 0 };
	s7_pointer		code = s7_is_pair(s7_cdr(item)) ? s7_cadr(item) : NULL;
	const char*		error;

	if (!code ||
	    !s7_is_integer(code) ||
	    s7_integer(code) < 1 ||
	    s7_integer(code) > 127)
		return form_fail(ctx, "%s: expected (function-code CODE ...) with CODE 1-127",
				 form_text(ctx, item));
	fc.code = (u8)s7_integer(code);
	if (config_fc_is_builtin(fc.code))
		return form_fail(ctx, "%s: function code %u (0x%02X) is implemented by mogbus and cannot be "
				 "redefined", form_text(ctx, item), fc.code, fc.code);

	for (s7_pointer p = s7_cddr(item); s7_is_pair(p); p = s7_cdr(p))
	{
		s7_pointer			entry = s7_car(p);
		const char*			head  = form_head_name(entry);
		struct device_custom_case_t*	c;

		if (head && strcmp(head, "name") == 0)
		{
			const char* name = NULL;

			if (!form_get_string(ctx, entry, &name))
				return false;
			if (strlen(name) >= sizeof(fc.name))
				return form_fail(ctx, "%s: name is too long (max %u characters)",
						 form_text(ctx, entry), (u32)sizeof(fc.name) - 1);
			strcpy(fc.name, name);
			continue;
		}
		if (fc.case_count >= DEVICE_MAX_CUSTOM_CASES)
			return form_fail(ctx, "%s: too many cases (max %d)", form_text(ctx, item),
					 DEVICE_MAX_CUSTOM_CASES);
		c = &fc.cases[fc.case_count];
		if (head && strcmp(head, "on") == 0)
		{
			u32 n = 0;

			if (s7_list_length(ctx->sc, entry) != 3)
				return form_fail(ctx, "%s: expected (on \"HEX PREFIX\" ACTION)", form_text(ctx, entry));
			if (!parse_hex_bytes(ctx, s7_cadr(entry), "on", c->prefix, DEVICE_CUSTOM_PREFIX_MAX, &n) ||
			    !parse_custom_action(ctx, s7_caddr(entry), c))
				return false;
			c->prefix_len = (u8)n;
		}
		else if (!parse_custom_action(ctx, entry, c))	/* a bare action matches every request */
			return false;
		fc.case_count++;
	}
	if (fc.case_count == 0)
		return form_fail(ctx, "%s: needs at least one (on \"HEX\" ACTION) or ACTION",
				 form_text(ctx, item));
	error = device_add_custom_fc(dev, &fc);
	if (error)
		return form_fail(ctx, "%s: %s", form_text(ctx, item), error);
	return true;
}

static bool
parse_machine(form_ctx_t* ctx, s7_pointer item, device_t* dev)
{
	return config_machine_add(ctx, item, dev);
}

/* --- device --- */

/** @brief Parses one item form of a `(device ...)` section into the device. */
typedef bool (*item_parser_t)(form_ctx_t*, s7_pointer, device_t*);

/** @brief Maps the head symbol of a device item form to its parser. */
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
	{ "server-id-additional-data", parse_server_id_additional_data },
	{ "com-status",       parse_com_status       },
	{ "com-event",        parse_com_event        },
	{ "vendor-name",      parse_vendor_name      },
	{ "product-code",     parse_product_code     },
	{ "major-minor-revision", parse_major_minor_revision },
	{ "vendor-url",       parse_vendor_url       },
	{ "product-name",     parse_product_name     },
	{ "model-name",       parse_model_name       },
	{ "user-application-name", parse_user_application_name },
	{ "device-id-object", parse_device_id_object  },
	{ "fault",            parse_fault            },
	{ "note",             parse_note             },
	{ "coil",             parse_coil             },
	{ "discrete",         parse_discrete         },
	{ "input",            parse_input            },
	{ "holding",          parse_holding          },
	{ "file",             parse_file             },
	{ "fifo",             parse_fifo             },
	{ "canopen-object",   parse_canopen_object   },
	{ "function-code",    parse_function_code    },
	{ "machine",          parse_machine          },
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
				 "run-indicator, server-id-additional-data, com-status, com-event, vendor-name, "
				 "product-code, major-minor-revision, vendor-url, product-name, model-name, "
				 "user-application-name, device-id-object, fault, note, coil, discrete, input, "
				 "holding, file, fifo, canopen-object or machine", head);

	return entry->parse(ctx, item, dev);
}

bool
config_parse_device(form_ctx_t* ctx, s7_pointer form, arena_t* arena,
		    u64 start_ms, device_t** out)
{
	device_t*	dev         = device_new(arena, start_ms);
	bool		has_unit_id = false;

	if (!dev)
		return form_fail(ctx, "out of memory while creating a device");

	strcpy(dev->name, "unnamed");
	for (s7_pointer p = s7_cdr(form); s7_is_pair(p); p = s7_cdr(p))
	{
		const char* head = form_head_name(s7_car(p));

		if (head && strcmp(head, "unit-id") == 0)
		{
			if (has_unit_id)
				return form_fail(ctx, "device '%s': unit-id is given more than once", dev->name);
			has_unit_id = true;
		}
		if (!parse_item(ctx, s7_car(p), dev))
			return false;
	}
	/* Clients address devices by unit id, so it is never implied. */
	if (!has_unit_id)
		return form_fail(ctx, "device '%s': missing (unit-id N); every device must set its Modbus unit id",
				 dev->name);

	for (u32 i = 0; i < dev->point_count; i++)
	{
		const point_t*	point = &dev->points[i];
		u32		target = 0;

		if (!model_needs_target(&point->model))
			continue;

		target = device_find_point(dev, (space_kind_t)point->model.target_space,
					   point->model.target_address);
		if (target == DEVICE_MAX_POINTS)
			return form_fail(ctx, "device '%s': %s %u: lag target %s %u is not the first address of a configured point",
					 dev->name, space_name(point->space), point->address,
					 SPACE_NAMES[point->model.target_space], point->model.target_address);
		if (target == i)
			return form_fail(ctx, "device '%s': %s %u: a lag cannot follow itself",
					 dev->name, space_name(point->space), point->address);
	}
	device_reset_models(dev, dev->start_ms);

	if (!config_machine_check_points(ctx, dev))
		return false;

	*out = dev;
	return true;
}
