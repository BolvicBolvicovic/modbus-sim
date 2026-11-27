/**
 * @file
 * @brief Public C API (include/mogbus.h): a thin top layer over config (optional), device,
 *        server and util.
 *
 * Nothing else includes this module. Handles are structs pushed on the context arena, so the
 * API itself never calls malloc.
 */
#include "mogbus.h"
#include "build_config.h"
#include "device/device.h"
#include "net/rtu_framer.h"
#include "os/os_console.h"
#include "os/os_socket.h"
#include "os/os_time.h"
#include "server/server.h"
#include "server/server_limits.h"
#include "util/log.h"
#include "vlib/arena.h"
#if WITH_CONFIG
#include "config/config.h"
#include "server/server_config.h"
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** @brief The public enums are mapped by value, so they must match the internal ones. */
_Static_assert((int)MOGBUS_COIL == (int)SPACE_COIL &&
	       (int)MOGBUS_DISCRETE == (int)SPACE_DISCRETE &&
	       (int)MOGBUS_INPUT == (int)SPACE_INPUT &&
	       (int)MOGBUS_HOLDING == (int)SPACE_HOLDING &&
	       (int)MOGBUS_SPACE_COUNT == (int)SPACE_COUNT, "space enum");
_Static_assert((int)MOGBUS_TYPE_U16 == (int)VT_U16 && (int)MOGBUS_TYPE_S16 == (int)VT_S16 &&
	       (int)MOGBUS_TYPE_U32 == (int)VT_U32 && (int)MOGBUS_TYPE_S32 == (int)VT_S32 &&
	       (int)MOGBUS_TYPE_F32 == (int)VT_F32 && (int)MOGBUS_TYPE_BOOL == (int)VT_BOOL &&
	       (int)MOGBUS_TYPE_U64 == (int)VT_U64 && (int)MOGBUS_TYPE_S64 == (int)VT_S64 &&
	       (int)MOGBUS_TYPE_F64 == (int)VT_F64 && (int)MOGBUS_TYPE_STRING == (int)VT_STRING,
	       "value type enum");
_Static_assert((int)MOGBUS_BYTES_BIG == (int)BYTES_BIG &&
	       (int)MOGBUS_BYTES_LITTLE == (int)BYTES_LITTLE, "byte order enum");
_Static_assert((int)MOGBUS_WORDS_HIGH_FIRST == (int)WORDS_HIGH_FIRST &&
	       (int)MOGBUS_WORDS_LOW_FIRST == (int)WORDS_LOW_FIRST, "word order enum");
_Static_assert((int)MOGBUS_POINT_STATIC == (int)POINT_STATIC &&
	       (int)MOGBUS_POINT_RAMP == (int)POINT_RAMP, "point kind enum");
_Static_assert((int)MOGBUS_RAMP_WRAP == (int)RAMP_WRAP &&
	       (int)MOGBUS_RAMP_BOUNCE == (int)RAMP_BOUNCE &&
	       (int)MOGBUS_RAMP_HOLD == (int)RAMP_HOLD, "ramp mode enum");
_Static_assert((int)MOGBUS_MODE_TCP == (int)SERVER_MODE_TCP &&
	       (int)MOGBUS_MODE_RTU_PTY == (int)SERVER_MODE_RTU_PTY &&
	       (int)MOGBUS_MODE_RTU_DEVICE == (int)SERVER_MODE_RTU_DEVICE &&
	       (int)MOGBUS_MODE_RTU_TCP == (int)SERVER_MODE_RTU_TCP &&
	       (int)MOGBUS_MODE_ASCII_PTY == (int)SERVER_MODE_ASCII_PTY &&
	       (int)MOGBUS_MODE_ASCII_DEVICE == (int)SERVER_MODE_ASCII_DEVICE &&
	       (int)MOGBUS_MODE_ASCII_TCP == (int)SERVER_MODE_ASCII_TCP &&
	       (int)MOGBUS_MODE_RTU_FD == (int)SERVER_MODE_RTU_FD &&
	       (int)MOGBUS_MODE_ASCII_FD == (int)SERVER_MODE_ASCII_FD &&
	       (int)MOGBUS_MODE_UDP == (int)SERVER_MODE_UDP &&
	       (int)MOGBUS_MODE_ASCII_UDP == (int)SERVER_MODE_ASCII_UDP &&
	       (int)MOGBUS_MODE_RTU_UDP == (int)SERVER_MODE_RTU_UDP, "mode enum");
_Static_assert((int)MOGBUS_PARITY_NONE == (int)PARITY_NONE &&
	       (int)MOGBUS_PARITY_EVEN == (int)PARITY_EVEN &&
	       (int)MOGBUS_PARITY_ODD == (int)PARITY_ODD &&
	       (int)MOGBUS_PARITY_MARK == (int)PARITY_MARK &&
	       (int)MOGBUS_PARITY_SPACE == (int)PARITY_SPACE, "parity enum");
_Static_assert((int)MOGBUS_TLS_1_2 == (int)NET_TLS_VERSION_1_2 &&
	       (int)MOGBUS_TLS_1_3 == (int)NET_TLS_VERSION_1_3, "tls version enum");
_Static_assert((int)MOGBUS_LOG_DEBUG == (int)LOG_LEVEL_DEBUG &&
	       (int)MOGBUS_LOG_INFO == (int)LOG_LEVEL_INFO &&
	       (int)MOGBUS_LOG_WARN == (int)LOG_LEVEL_WARN &&
	       (int)MOGBUS_LOG_OFF == (int)LOG_LEVEL_OFF, "log level enum");
_Static_assert((int)MOGBUS_FAULT_DELAY == (int)FAULT_DELAY &&
	       (int)MOGBUS_FAULT_DROP == (int)FAULT_DROP &&
	       (int)MOGBUS_FAULT_EXCEPTION == (int)FAULT_EXCEPTION &&
	       (int)MOGBUS_FAULT_CORRUPT == (int)FAULT_CORRUPT &&
	       (int)MOGBUS_FAULT_DISCONNECT == (int)FAULT_DISCONNECT &&
	       (int)MOGBUS_FAULT_RESTART == (int)FAULT_RESTART &&
	       (int)MOGBUS_FAULT_SENSOR_DISCONNECT == (int)FAULT_SENSOR_DISCONNECT &&
	       (int)MOGBUS_FAULT_KIND_COUNT == (int)FAULT_KIND_COUNT, "fault kind enum");
_Static_assert((int)MOGBUS_STEP_CONTINUE == (int)NET_STEP_CONTINUE &&
	       (int)MOGBUS_STEP_DONE == (int)NET_STEP_DONE &&
	       (int)MOGBUS_STEP_ERROR == (int)NET_STEP_ERROR, "step results");
_Static_assert((int)MOGBUS_MAX_DEVICES == (int)SERVER_MAX_DEVICES, "device limit");
_Static_assert(sizeof(((mogbus_device_info_t*)0)->custom_fcs) /
	       sizeof(((mogbus_device_info_t*)0)->custom_fcs[0]) == DEVICE_MAX_CUSTOM_FCS,
	       "custom function code limit");
_Static_assert((int)MOGBUS_LOG_TEXT == (int)LOG_FORMAT_TEXT &&
	       (int)MOGBUS_LOG_JSON == (int)LOG_FORMAT_JSON, "log format enum");

#define VERSION_STRING_(a, b, c)	#a "." #b "." #c
#define VERSION_STRING(a, b, c)		VERSION_STRING_(a, b, c)

/**
 * @brief A server slot.
 *
 * Slots are reused after close because arena memory cannot be freed one by one. Strings of the
 * options are copied into `strings` (mogbus_server_check() validates into a stack copy of the
 * same struct).
 */
struct mogbus_server_t
{
	server_t	server;
	mogbus_t*	ctx;
	bool		in_use;
	/** @brief Copies of the option strings, so they outlive the caller's buffers. */
	struct strings_t
	{
		char	bind[SERVER_BIND_MAX];
		char	http_bind[SERVER_BIND_MAX];
		char	path[SERVER_PATH_MAX];
		char	certificate[SERVER_TLS_PATH_MAX];
		char	private_key[SERVER_TLS_PATH_MAX];
		char	ca_certificate[SERVER_TLS_PATH_MAX];
		char	tls1_2_ciphers[SERVER_TLS_CIPHER_MAX];
		char	tls1_3_ciphersuites[SERVER_TLS_CIPHER_MAX];
	} strings;
};

/** @brief State of one context: its arena, the loaded config, devices and server slots. */
struct mogbus_t
{
	arena_t*	arena;
#if WITH_CONFIG
	config_t	config;		/**< last loaded file: server section only is used */
	bool		has_config;
#endif
	device_t*	devices[SERVER_MAX_DEVICES];
	u32		device_count;
	mogbus_server_t* servers[MOGBUS_MAX_SERVERS];
};

static void __attribute__((format(printf, 3, 4)))
set_err(char* err, size_t err_size, const char* fmt, ...)
{
	va_list	args;

	if (!err || err_size == 0)
		return;
	va_start(args, fmt);
	vsnprintf(err, err_size, fmt, args);
	va_end(args);
}

const char*
mogbus_version(void)
{
	return VERSION_STRING(MOGBUS_VERSION_MAJOR, MOGBUS_VERSION_MINOR, MOGBUS_VERSION_PATCH);
}

mogbus_t*
mogbus_create(void)
{
	arena_t*	arena = ARENA_ALLOC();
	mogbus_t*	ctx   = NULL;

	if (!arena)
		return NULL;
	ctx = ARENA_PUSH_ARRAY_ALIGNED(arena, mogbus_t, 1, 16);
	if (!ctx)
	{
		arena_release(arena);
		return NULL;
	}
	ctx->arena = arena;
	return ctx;
}

void
mogbus_destroy(mogbus_t* ctx)
{
	if (!ctx)
		return;
	for (u32 i = 0; i < MOGBUS_MAX_SERVERS; i++)
	{
		if (ctx->servers[i] && ctx->servers[i]->in_use)
			mogbus_server_close(ctx->servers[i]);
	}
	arena_release(ctx->arena);	/* the context lives in its own arena */
}

static device_t*
find_device(const mogbus_t* ctx, u8 unit)
{
	for (u32 i = 0; i < ctx->device_count; i++)
	{
		if (ctx->devices[i]->unit_id == unit)
			return ctx->devices[i];
	}
	return NULL;
}

#if WITH_CONFIG
bool
mogbus_load_config(mogbus_t* ctx, const char* path, char* err, size_t err_size)
{
	char		local[512];
	config_t	loaded;
	arena_t*	scratch = ARENA_ALLOC();
	u32		pos     = arena_pos(ctx->arena);
	bool		ok      = false;

	if (!err || err_size == 0)
	{
		err      = local;
		err_size = sizeof(local);
	}
	if (!scratch)
	{
		set_err(err, err_size, "out of memory allocating scratch arena");
		return false;
	}
	if (!config_load(ctx->arena, scratch, path, &loaded, err, (u32)err_size))
		goto done;
	if (ctx->device_count + loaded.device_count > SERVER_MAX_DEVICES)
	{
		set_err(err, err_size, "too many devices (max %d per context)", SERVER_MAX_DEVICES);
		goto done;
	}
	for (u32 i = 0; i < loaded.device_count; i++)
	{
		if (find_device(ctx, loaded.devices[i]->unit_id))
		{
			set_err(err, err_size, "unit id %u is already used by another device",
				loaded.devices[i]->unit_id);
			goto done;
		}
	}
	for (u32 i = 0; i < loaded.device_count; i++)
	{
		if (!loaded.devices[i]->any_unit)
			continue;
		for (u32 j = 0; j < ctx->device_count; j++)
		{
			if (!ctx->devices[j]->any_unit)
				continue;
			set_err(err, err_size, "devices '%s' and '%s' both set any-unit; only one device may answer every unit id",
				ctx->devices[j]->name, loaded.devices[i]->name);
			goto done;
		}
	}
	for (u32 i = 0; i < loaded.device_count; i++)
	{
		loaded.devices[i]->owner = ctx;
		ctx->devices[ctx->device_count++] = loaded.devices[i];
	}
	ctx->config     = loaded;
	ctx->has_config = true;
	ok = true;

done:
	/* Give back what a rejected load took from the context arena. */
	if (!ok)
		arena_pop_to(ctx->arena, pos);
	arena_release(scratch);
	return ok;
}
#else
bool
mogbus_load_config(mogbus_t* ctx, const char* path, char* err, size_t err_size)
{
	(void)ctx;
	(void)path;
	set_err(err, err_size, "built without config support");
	return false;
}
#endif

mogbus_device_t*
mogbus_add_device(mogbus_t* ctx, uint8_t unit_id, const char* name, char* err, size_t err_size)
{
	device_t*	dev = NULL;

	if (ctx->device_count >= SERVER_MAX_DEVICES)
	{
		set_err(err, err_size, "too many devices (max %d per context)", SERVER_MAX_DEVICES);
		return NULL;
	}
	if (find_device(ctx, unit_id))
	{
		set_err(err, err_size, "unit id %u is already used by another device", unit_id);
		return NULL;
	}
	if (name && strlen(name) >= DEVICE_NAME_MAX)
	{
		set_err(err, err_size, "device name is too long (max %d bytes)", DEVICE_NAME_MAX - 1);
		return NULL;
	}
	dev = device_new(ctx->arena, os_now_ms());
	if (!dev)
	{
		set_err(err, err_size, "out of memory while creating a device");
		return NULL;
	}
	dev->unit_id = unit_id;
	dev->owner   = ctx;
	strcpy(dev->name, name ? name : "unnamed");
	ctx->devices[ctx->device_count++] = dev;
	return (mogbus_device_t*)dev;
}

mogbus_device_t*
mogbus_find_device(mogbus_t* ctx, uint8_t unit_id)
{
	return (mogbus_device_t*)find_device(ctx, unit_id);
}

size_t
mogbus_device_count(const mogbus_t* ctx)
{
	return ctx->device_count;
}

mogbus_device_t*
mogbus_device_at(mogbus_t* ctx, size_t index)
{
	return index < ctx->device_count ? (mogbus_device_t*)ctx->devices[index] : NULL;
}

bool
mogbus_device_info(const mogbus_device_t* handle, mogbus_device_info_t* out)
{
	const device_t*	dev = (const device_t*)handle;

	if (!dev || !out)
		return false;
	memset(out, 0, sizeof(*out));
	out->name    = dev->name;
	out->unit_id = dev->unit_id;
	out->any_unit = dev->any_unit;
	for (u32 i = 0; i < dev->point_count; i++)
		out->point_count[dev->points[i].space]++;
	for (u32 i = 0; i < FAULT_KIND_COUNT; i++)
	{
		const fault_t* fault = &dev->faults.by_kind[i];

		if ((dev->faults.present & FAULT_BIT(i)) == 0)
			continue;
		out->faults[i].present        = true;
		out->faults[i].scheduled      = fault->schedule_id != 0;
		out->faults[i].percent        = fault->percent;
		out->faults[i].delay_ms       = fault->delay_ms;
		out->faults[i].exception_code = fault->exception_code;
	}
	out->custom_fc_count = dev->custom_fc_count;
	for (u32 i = 0; i < dev->custom_fc_count; i++)
	{
		out->custom_fcs[i].code = dev->custom_fcs[i].code;
		out->custom_fcs[i].name = dev->custom_fcs[i].name;
	}
	return true;
}

void
mogbus_rtu_timing(uint32_t baud, uint32_t frame_gap_ms, uint32_t* t35_us, uint32_t* gap_ms)
{
	if (t35_us)
		*t35_us = rtu_t35_us(baud);
	if (gap_ms)
		*gap_ms = rtu_effective_gap_ms(frame_gap_ms, baud);
}

bool
mogbus_device_add_point(mogbus_device_t* handle, const mogbus_point_t* in, char* err, size_t err_size)
{
	device_t*	dev   = (device_t*)handle;
	point_t		point = { 0 };
	const char*	error = NULL;

	if ((unsigned)in->space >= SPACE_COUNT ||
	    (unsigned)in->type > MOGBUS_TYPE_STRING ||
	    (unsigned)in->word_order > MOGBUS_WORDS_LOW_FIRST ||
	    (unsigned)in->byte_order > MOGBUS_BYTES_LITTLE ||
	    (unsigned)in->kind > MOGBUS_POINT_CLOCK ||
	    (unsigned)in->clock_field > MOGBUS_CLOCK_UNIX ||
	    (unsigned)in->ramp.mode > MOGBUS_RAMP_HOLD)
	{
		set_err(err, err_size, "point has an invalid space, type, word order, byte order, kind, clock field or ramp mode");
		return false;
	}
	point.space   = (space_kind_t)in->space;
	point.address = in->address;
	point.type    = (value_type_t)in->type;
	point.order   = (word_order_t)in->word_order;
	point.byte_order = (byte_order_t)in->byte_order;
	point.read_only  = in->read_only;
	if (point.type == VT_STRING)
	{
		if (in->length > DEVICE_MAX_STRING_REGISTERS)
		{
			set_err(err, err_size, "string length must be between 1 and 123 registers");
			return false;
		}
		point.length = (u16)in->length;
		point.text   = in->text;
	}
	point.kind    = in->kind == MOGBUS_POINT_CLOCK ? POINT_CLOCK : (point_kind_t)in->kind;
	point.clock_field = (clock_field_t)in->clock_field;
	point.clock_utc   = in->clock_utc;
	if (in->kind == MOGBUS_POINT_CLOCK)
		point.static_value = 0;
	else if (in->kind == MOGBUS_POINT_RAMP)
	{
		if (space_is_bit(point.space))
		{
			set_err(err, err_size, "coils and discrete inputs can only be static");
			return false;
		}
		point.ramp = (ramp_params_t){
			.from     = in->ramp.from,
			.to       = in->ramp.to,
			.step     = in->ramp.step,
			.every_ms = in->ramp.every_ms,
			.mode     = (ramp_mode_t)in->ramp.mode,
		};
	}
	else if (space_is_bit(point.space))
		point.static_value = in->value != 0 ? 1.0 : 0.0;
	else
	{
		if (!isfinite(in->value) &&
		    point.type != VT_F32 &&
		    point.type != VT_F64)
		{
			set_err(err, err_size, "only float32 and float64 static values can be NaN or infinity");
			return false;
		}
		point.static_value = in->value;
	}

	error = device_add_point(dev, &point);
	if (error)
		set_err(err, err_size, "%s", error);
	return error == NULL;
}

bool
mogbus_device_read(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
		   uint32_t count, uint16_t* out)
{
	device_t*	dev = (device_t*)handle;

	if ((unsigned)space >= SPACE_COUNT || count == 0 || !out)
		return false;
	device_tick(dev, os_now_ms());
	return device_read(dev, (space_kind_t)space, address, count, out);
}

bool
mogbus_device_write(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
		    uint32_t count, const uint16_t* in)
{
	device_t*	dev = (device_t*)handle;

	if ((unsigned)space >= SPACE_COUNT || count == 0 || !in)
		return false;
	if (space_is_bit((space_kind_t)space))
	{
		for (u32 i = 0; i < count; i++)
		{
			if (in[i] > 1)
				return false;
		}
	}
	device_tick(dev, os_now_ms());
	return device_write(dev, (space_kind_t)space, address, count, in);
}

bool
mogbus_device_get_value(mogbus_device_t* handle, mogbus_space_t space, uint32_t address, double* out)
{
	device_t*	dev = (device_t*)handle;
	val_t		val;

	if ((unsigned)space >= SPACE_COUNT || !out)
		return false;
	device_tick(dev, os_now_ms());
	if (!device_get_point(dev, (space_kind_t)space, address, &val))
		return false;
	*out = val.type == VAL_BOOL ? (val.b ? 1.0 : 0.0) : val.n;
	return true;
}

bool
mogbus_device_set_value(mogbus_device_t* handle, mogbus_space_t space, uint32_t address, double value)
{
	device_t*	dev = (device_t*)handle;
	val_t		val = { 0 };

	if ((unsigned)space >= SPACE_COUNT)
		return false;
	device_tick(dev, os_now_ms());
	if (space_is_bit((space_kind_t)space))
	{
		val.type = VAL_BOOL;
		val.b    = value != 0;
	}
	else
	{
		val.type = VAL_NUM;
		val.n    = value;
	}
	return device_set_point(dev, (space_kind_t)space, address, &val);
}

bool
mogbus_device_get_string(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
			 char* out, size_t size)
{
	if ((unsigned)space >= SPACE_COUNT)
		return false;
	return device_get_string((device_t*)handle, (space_kind_t)space, address, out, size);
}

/** @brief The host's hook and user pointer of one device, the `user` of its trampoline. */
struct access_hook_t
{
	mogbus_access_fn fn;
	void*		user;
};

/* Device-level hook: maps the layer-neutral access onto the public one. */
static u8
access_trampoline(void* user, device_t* dev, const device_access_t* in)
{
	const struct access_hook_t*	hook   = user;
	mogbus_access_t			access = {
		.kind          = in->write ? MOGBUS_ACCESS_WRITE : MOGBUS_ACCESS_READ,
		.space         = (mogbus_space_t)in->space,
		.address       = in->address,
		.count         = in->count,
		.values        = in->values,
		.function_code = in->function_code,
	};

	return hook->fn(hook->user, (mogbus_device_t*)dev, &access);
}

bool
mogbus_device_set_any_unit(mogbus_device_t* handle, bool on)
{
	device_t*	dev = (device_t*)handle;
	const mogbus_t*	ctx = dev ? dev->owner : NULL;

	if (!ctx)
		return false;
	for (u32 i = 0; on && i < ctx->device_count; i++)
	{
		if (ctx->devices[i] != dev &&
		    ctx->devices[i]->any_unit)
			return false;
	}
	dev->any_unit = on;
	return true;
}

bool
mogbus_device_set_access_hook(mogbus_device_t* handle, mogbus_access_fn fn, void* user)
{
	device_t*		dev  = (device_t*)handle;
	struct access_hook_t*	hook = NULL;

	if (!dev)
		return false;
	if (!fn)
	{
		dev->access_fn   = NULL;
		dev->access_user = NULL;
		return true;
	}
	/* The holder is allocated once per device and reused by later calls. */
	hook = dev->access_fn == access_trampoline ? dev->access_user : ARENA_PUSH_ARRAY(dev->arena, struct access_hook_t, 1);
	if (!hook)
		return false;
	hook->fn         = fn;
	hook->user       = user;
	dev->access_fn   = access_trampoline;
	dev->access_user = hook;
	return true;
}

bool
mogbus_device_set_string(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
			 const char* text)
{
	if ((unsigned)space >= SPACE_COUNT)
		return false;
	return device_set_string((device_t*)handle, (space_kind_t)space, address, text);
}

mogbus_server_options_t
mogbus_server_options_defaults(void)
{
	return (mogbus_server_options_t){
		.mode                           = MOGBUS_MODE_TCP,
		.http_port                      = SERVER_DEFAULT_HTTP_PORT,
		.bind_address                   = "127.0.0.1",
		.port                           = SERVER_DEFAULT_PORT,
		.fd                             = -1,
		.idle_timeout_ms                = SERVER_DEFAULT_IDLE_TIMEOUT_MS,
		.baud                           = SERVER_DEFAULT_BAUD,
		.data_bits                      = 8,
		.parity                         = MOGBUS_PARITY_NONE,
		.stop_bits                      = 1,
		.tls_handshake_timeout_ms       = SERVER_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS,
		.tls_require_client_certificate = true,
		.tls_min_version                = MOGBUS_TLS_1_2,
	};
}

/** @brief Copies src into dst; NULL stays NULL and a too-long string fails with *ok = false. */
static const char*
copy_string(char* dst, size_t cap, const char* src, const char* what, bool* ok,
	    char* err, size_t err_size)
{
	if (!src)
		return NULL;
	if (strlen(src) >= cap)
	{
		set_err(err, err_size, "%s is too long (max %zu bytes)", what, cap - 1);
		*ok = false;
		return NULL;
	}
	strcpy(dst, src);
	return dst;
}

/**
 * @brief The validation shared by create and mogbus_server_check(): fails for no devices, a
 *        missing path or fd, a bad serial line, over-long strings and unusable TLS files.
 *
 * `o` borrows its strings; they are copied into `st`, so the caller's storage may go away, and
 * `o` is repointed at the copies. Opens nothing.
 */
static mogbus_error_t
validate_options(const mogbus_t* ctx, server_options_t* o, struct strings_t* st,
		 char* err, size_t err_size)
{
	bool ok = true;

	if (ctx->device_count == 0)
	{
		set_err(err, err_size, "the context has no devices to serve");
		return MOGBUS_ERR_INVALID;
	}
	if ((o->mode == SERVER_MODE_RTU_DEVICE || o->mode == SERVER_MODE_ASCII_DEVICE) &&
	    (!o->path || !o->path[0]))
	{
		set_err(err, err_size, "this mode needs a device path");
		return MOGBUS_ERR_INVALID;
	}
	if ((o->mode == SERVER_MODE_RTU_FD || o->mode == SERVER_MODE_ASCII_FD) && o->fd < 0)
	{
		set_err(err, err_size, "this mode needs an open fd");
		return MOGBUS_ERR_INVALID;
	}
	if ((o->data_bits != 7 && o->data_bits != 8) ||
	    (o->stop_bits != 1 && o->stop_bits != 2) ||
	    (unsigned)o->parity > PARITY_SPACE)
	{
		set_err(err, err_size, "serial line needs 7 or 8 data bits, 1 or 2 stop bits and a valid parity");
		return MOGBUS_ERR_INVALID;
	}

	o->bind_address = copy_string(st->bind, sizeof(st->bind), o->bind_address, "bind address",
				      &ok, err, err_size);
	o->http_bind_address = copy_string(st->http_bind, sizeof(st->http_bind), o->http_bind_address,
					   "HTTP bind address", &ok, err, err_size);
	if (ok && o->http_bind_address && !os_ip_is_literal(o->http_bind_address))
	{
		set_err(err, err_size, "HTTP bind address '%s' is not a valid IPv4 or IPv6 address",
			o->http_bind_address);
		return MOGBUS_ERR_INVALID;
	}
	o->path         = copy_string(st->path, sizeof(st->path), o->path, "device path", &ok, err, err_size);
	o->tls.certificate = copy_string(st->certificate, sizeof(st->certificate), o->tls.certificate,
					 "TLS certificate path", &ok, err, err_size);
	o->tls.private_key = copy_string(st->private_key, sizeof(st->private_key), o->tls.private_key,
					 "TLS private key path", &ok, err, err_size);
	o->tls.ca_certificate = copy_string(st->ca_certificate, sizeof(st->ca_certificate),
					    o->tls.ca_certificate, "TLS CA path", &ok, err, err_size);
	o->tls.tls1_2_ciphers = copy_string(st->tls1_2_ciphers, sizeof(st->tls1_2_ciphers),
					    o->tls.tls1_2_ciphers, "TLS 1.2 cipher list", &ok, err, err_size);
	o->tls.tls1_3_ciphersuites = copy_string(st->tls1_3_ciphersuites, sizeof(st->tls1_3_ciphersuites),
						 o->tls.tls1_3_ciphersuites, "TLS 1.3 ciphersuites",
						 &ok, err, err_size);
	if (!ok)
		return MOGBUS_ERR_INVALID;
	if (o->tls_enabled && !server_check_tls(o, err, (u32)err_size))
		return MOGBUS_ERR_LISTEN;
	return MOGBUS_OK;
}

/** @brief Common tail of both create functions. */
static mogbus_server_t*
create_server(mogbus_t* ctx, server_options_t o, char* err, size_t err_size, mogbus_error_t* status)
{
	mogbus_server_t*	s     = NULL;
	char			local[256];
	mogbus_error_t		error = MOGBUS_ERR_INVALID;
	s32			rc    = 0;

	if (!err || err_size == 0)
	{
		err      = local;
		err_size = sizeof(local);
	}
	for (u32 i = 0; i < MOGBUS_MAX_SERVERS && !s; i++)
	{
		if (!ctx->servers[i])
			ctx->servers[i] = ARENA_PUSH_ARRAY_ALIGNED(ctx->arena, mogbus_server_t, 1, 16);
		if (ctx->servers[i] && !ctx->servers[i]->in_use)
			s = ctx->servers[i];
		else if (!ctx->servers[i])
		{
			set_err(err, err_size, "out of memory while creating a server");
			goto fail;
		}
	}
	if (!s)
	{
		set_err(err, err_size, "too many servers (max %d per context)", MOGBUS_MAX_SERVERS);
		goto fail;
	}

	error = validate_options(ctx, &o, &s->strings, err, err_size);
	if (error != MOGBUS_OK)
	{
		/* A server that fails on TLS has always logged it; keep that for log readers. */
		if (error == MOGBUS_ERR_LISTEN)
			LOG_WARN("cannot configure TLS: %s", err);
		goto fail;
	}

	o.devices      = ctx->devices;
	o.device_count = ctx->device_count;
	rc = server_init(&s->server, &o);
	if (rc != 0)
	{
		error = rc == 1 ? MOGBUS_ERR_SERIAL : MOGBUS_ERR_LISTEN;
		set_err(err, err_size, rc == 1 ? "cannot open the serial device or PTY" :
			"cannot listen on %s:%u (address in use, invalid, or TLS setup failed)",
			o.bind_address ? o.bind_address : "", o.port);
		goto fail;
	}
	s->ctx    = ctx;
	s->in_use = true;
	if (status)
		*status = MOGBUS_OK;
	return s;

fail:
	if (status)
		*status = error;
	return NULL;
}

/** @brief Public options to internal ones; false with a message for an enum out of range. */
static bool
options_from_public(const mogbus_server_options_t* in, server_options_t* o, char* err, size_t err_size)
{
	if ((unsigned)in->mode > MOGBUS_MODE_RTU_UDP ||
	    (unsigned)in->parity > MOGBUS_PARITY_SPACE ||
	    (unsigned)in->tls_min_version > MOGBUS_TLS_1_3)
	{
		set_err(err, err_size, "invalid mode, parity or TLS version");
		return false;
	}
	*o = (server_options_t){
		.mode                     = (server_mode_t)in->mode,
		.bind_address             = in->bind_address ? in->bind_address : "127.0.0.1",
		.http_bind_address        = in->http_bind_address,
		.http_port                = in->http_port,
		.http_control             = in->http_control,
		.port                     = in->port,
		.path                     = in->path,
		.fd                       = in->fd,
		.seed                     = in->seed,
		.idle_timeout_ms          = in->idle_timeout_ms,
		.frame_gap_ms             = in->frame_gap_ms,
		.baud                     = in->baud,
		.strict_t15               = in->strict_t15,
		.broadcast_execute        = in->broadcast_execute,
		.data_bits                = in->data_bits,
		.parity                   = (parity_t)in->parity,
		.stop_bits                = in->stop_bits,
		.tls_enabled              = in->tls_enabled,
		.tls_handshake_timeout_ms = in->tls_handshake_timeout_ms,
		.tls = {
			.certificate                = in->tls_certificate,
			.private_key                = in->tls_private_key,
			.ca_certificate             = in->tls_ca_certificate,
			.require_client_certificate = in->tls_require_client_certificate,
			.min_version                = (net_tls_version_t)in->tls_min_version,
			.tls1_2_ciphers             = in->tls_ciphers_1_2,
			.tls1_3_ciphersuites        = in->tls_ciphersuites_1_3,
		},
	};
	return true;
}

mogbus_server_t*
mogbus_server_create(mogbus_t* ctx, const mogbus_server_options_t* in, char* err, size_t err_size,
		     mogbus_error_t* status)
{
	server_options_t o;

	if (!options_from_public(in, &o, err, err_size))
	{
		if (status)
			*status = MOGBUS_ERR_INVALID;
		return NULL;
	}
	return create_server(ctx, o, err, err_size, status);
}

bool
mogbus_server_check(const mogbus_t* ctx, const mogbus_server_options_t* in, char* err, size_t err_size)
{
	server_options_t	o;
	struct strings_t	strings;
	char			local[256];

	if (!err || err_size == 0)
	{
		err      = local;
		err_size = sizeof(local);
	}
	return options_from_public(in, &o, err, err_size) &&
	       validate_options(ctx, &o, &strings, err, err_size) == MOGBUS_OK;
}

#if WITH_CONFIG
mogbus_server_t*
mogbus_server_create_from_config(mogbus_t* ctx, char* err, size_t err_size, mogbus_error_t* status)
{
	if (!ctx->has_config)
	{
		set_err(err, err_size, "no config loaded; call mogbus_load_config first");
		if (status)
			*status = MOGBUS_ERR_INVALID;
		return NULL;
	}
	return create_server(ctx, server_options_from_config(&ctx->config), err, err_size, status);
}

bool
mogbus_config_server_options(const mogbus_t* ctx, mogbus_server_options_t* out)
{
	/* The config lives in the (non-const) context; the options only read it. */
	server_options_t	o;

	if (!ctx->has_config)
		return false;
	o = server_options_from_config((config_t*)&ctx->config);
	*out = (mogbus_server_options_t){
		.mode                           = (mogbus_mode_t)o.mode,
		.bind_address                   = o.bind_address,
		.port                           = o.port,
		.http_bind_address              = o.http_bind_address,
		.http_port                      = o.http_port,
		.http_control                   = o.http_control,
		.path                           = o.path[0] ? o.path : NULL,
		.fd                             = -1,
		.seed                           = o.seed,
		.idle_timeout_ms                = o.idle_timeout_ms,
		.frame_gap_ms                   = o.frame_gap_ms,
		.baud                           = o.baud,
		.strict_t15                     = o.strict_t15,
		.broadcast_execute              = o.broadcast_execute,
		.data_bits                      = o.data_bits,
		.parity                         = (mogbus_parity_t)o.parity,
		.stop_bits                      = o.stop_bits,
		.tls_enabled                    = o.tls_enabled,
		.tls_handshake_timeout_ms       = o.tls_handshake_timeout_ms,
		.tls_certificate                = o.tls.certificate[0] ? o.tls.certificate : NULL,
		.tls_private_key                = o.tls.private_key[0] ? o.tls.private_key : NULL,
		.tls_ca_certificate             = o.tls.ca_certificate,
		.tls_require_client_certificate = o.tls.require_client_certificate,
		.tls_min_version                = (mogbus_tls_version_t)o.tls.min_version,
		.tls_ciphers_1_2                = o.tls.tls1_2_ciphers,
		.tls_ciphersuites_1_3           = o.tls.tls1_3_ciphersuites,
	};
	return true;
}
#else
mogbus_server_t*
mogbus_server_create_from_config(mogbus_t* ctx, char* err, size_t err_size, mogbus_error_t* status)
{
	(void)ctx;
	set_err(err, err_size, "built without config support");
	if (status)
		*status = MOGBUS_ERR_INVALID;
	return NULL;
}

bool
mogbus_config_server_options(const mogbus_t* ctx, mogbus_server_options_t* out)
{
	(void)ctx;
	(void)out;
	return false;
}
#endif

int
mogbus_server_step(mogbus_server_t* server, int max_wait_ms)
{
	return server_step(&server->server, max_wait_ms);
}

void
mogbus_server_stop(mogbus_server_t* server)
{
	server_stop(&server->server);
}

static mogbus_server_t* volatile* g_stop_slot;

static void
stop_from_signal(void)
{
	mogbus_server_t* server = g_stop_slot ? *g_stop_slot : NULL;

	if (server)
		mogbus_server_stop(server);
}

void
mogbus_stop_on_signals(mogbus_server_t* volatile* slot)
{
	g_stop_slot = slot;
	os_console_on_stop(stop_from_signal);
}

int
mogbus_server_run(mogbus_server_t* server)
{
	return server_serve(&server->server);
}

void
mogbus_server_close(mogbus_server_t* server)
{
	if (!server || !server->in_use)
		return;
	server_close(&server->server);
	server->in_use = false;
}

const char*
mogbus_server_pty_name(const mogbus_server_t* server)
{
	return server_pty_name(&server->server);
}

/** @brief The log sink is process-wide, like util/log itself. */
static struct
{
	mogbus_log_fn	fn;
	void*		user;
} g_log_sink;

static void
log_trampoline(void* ctx, log_level_t level, const char* message)
{
	(void)ctx;
	g_log_sink.fn(g_log_sink.user, (mogbus_log_level_t)level, message);
}

void
mogbus_set_log_sink(mogbus_log_fn fn, void* user)
{
	g_log_sink.fn   = fn;
	g_log_sink.user = user;
	log_set_sink(fn ? log_trampoline : NULL, NULL);
}

void
mogbus_set_log_level(mogbus_log_level_t level)
{
	log_set_level((log_level_t)level);
}

void
mogbus_set_log_format(mogbus_log_format_t format)
{
	log_set_format((log_format_t)format);
}

size_t
mogbus_server_metrics(const mogbus_server_t* server, char* out, size_t size)
{
	if (!server || !server->in_use || !out || size == 0)
		return 0;
	return net_obs_render_metrics(&server->server.obs, out, size > UINT32_MAX ? UINT32_MAX : (u32)size);
}
