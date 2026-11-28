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
#include "client/client.h"
#include "device/device.h"
#include "modbus/pdu.h"
#include "net/net_obs.h"
#include "net/rtu_framer.h"
#include "os/os_console.h"
#include "os/os_mutex.h"
#include "os/os_socket.h"
#include "os/os_time.h"
#include "server/server.h"
#include "util/log.h"
#include "view/view.h"
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
_Static_assert((int)VIEW_MAX_DEVICES == (int)SERVER_MAX_DEVICES, "view and context device limits differ");
_Static_assert(MOGBUS_MAX_POINTS == DEVICE_MAX_POINTS, "public and internal point limits differ");
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
	device_t*	devices[SERVER_MAX_DEVICES];	/**< the devices this server serves, fixed at creation */
	u32		device_count;
	mogbus_frame_fn	frame_fn;	/**< frame hook of the host, or NULL */
	void*		frame_user;
	mogbus_connection_fn conn_fn;	/**< connection hook of the host, or NULL */
	void*		conn_user;
	/** @brief Copies of the option strings, so they outlive the caller's buffers. */
	struct strings_t
	{
		char	id[SERVER_ID_MAX];
		char	bind[SERVER_BIND_MAX];
		char	http_bind[SERVER_BIND_MAX];
		char	http_hosts[NET_OBS_HOSTS_MAX];
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
	config_t	config;		/**< last loaded file: server sections only are used */
	bool		has_config;
#endif
	net_poll_group_t group;		/**< scratch of the shared wait of mogbus_step() */
	view_t		view;		/**< live register view, open while a server asked for it */
	device_t*	devices[SERVER_MAX_DEVICES];
	u32		device_count;
	mogbus_server_t* servers[MOGBUS_MAX_SERVERS];
	char		error[1024];	/**< message of the last failed call; empty if none */
	os_mutex_t	lock;		/**< recursive; held by every call that touches the context, released while a step waits */
};

/** @brief Locks a context (NULL is a no-op) and hands it back, for LOCK(). */
static mogbus_t*
lock_ctx(const mogbus_t* ctx)
{
	if (ctx)
		os_mutex_lock(&((mogbus_t*)ctx)->lock);
	return (mogbus_t*)ctx;
}

static void
unlock_ctx(mogbus_t** held)
{
	if (*held)
		os_mutex_unlock(&(*held)->lock);
}

/** @brief Holds the context lock until the end of the enclosing function, whatever way it returns. */
#define LOCK(ctx)	mogbus_t* lock_guard_ __attribute__((cleanup(unlock_ctx))) = lock_ctx(ctx)

/** @brief Records a failure message in the context and returns the code, so callers can `return fail(...)`. */
static mogbus_result_t __attribute__((format(printf, 3, 4)))
fail(mogbus_t* ctx, mogbus_result_t code, const char* fmt, ...)
{
	va_list	args;

	if (!ctx)
		return code;
	va_start(args, fmt);
	vsnprintf(ctx->error, sizeof(ctx->error), fmt, args);
	va_end(args);
	return code;
}

/** @brief The context a device handle belongs to; NULL for a NULL handle. */
static mogbus_t*
owner_of(const void* handle)
{
	return handle ? ((const device_t*)handle)->owner : NULL;
}

const char*
mogbus_last_error(const mogbus_t* ctx)
{
	return ctx ? ctx->error : client_detached_error();
}

const char*
mogbus_version(void)
{
	return VERSION_STRING(MOGBUS_VERSION_MAJOR, MOGBUS_VERSION_MINOR, MOGBUS_VERSION_PATCH);
}

mogbus_result_t
mogbus_create(mogbus_t** out)
{
	arena_t*	arena = ARENA_ALLOC();
	mogbus_t*	ctx   = NULL;

	*out = NULL;
	if (arena)
		ctx = ARENA_PUSH_ARRAY_ALIGNED(arena, mogbus_t, 1, 16);
	if (!ctx)
	{
		if (arena)
			arena_release(arena);
		client_detached_errorf("out of memory mapping the context arena");
		return MOGBUS_ERR_NOMEM;
	}
	if (!os_mutex_init(&ctx->lock))
	{
		arena_release(arena);
		client_detached_errorf("cannot create the context lock");
		return MOGBUS_ERR_NOMEM;
	}
	ctx->arena      = arena;
	ctx->group.lock = &ctx->lock;
	*out = ctx;
	return MOGBUS_OK;
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
	view_close(&ctx->view);
	os_mutex_destroy(&ctx->lock);
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

/** @brief The first device that has the unit id of `dev` and shares a server with it, or NULL. */
static device_t*
device_conflict(const mogbus_t* ctx, const device_t* dev)
{
	for (u32 i = 0; i < ctx->device_count; i++)
	{
		if (ctx->devices[i] != dev &&
		    ctx->devices[i]->unit_id == dev->unit_id &&
		    device_shares_server(ctx->devices[i], dev))
			return ctx->devices[i];
	}
	return NULL;
}

#if WITH_CONFIG
mogbus_result_t
mogbus_load_config(mogbus_t* ctx, const char* path)
{
	config_t	loaded;
	arena_t*	scratch = ARENA_ALLOC();
	u32		pos;
	mogbus_result_t	error   = MOGBUS_ERR_CONFIG;
	LOCK(ctx);

	pos = arena_pos(ctx->arena);
	if (!scratch)
		return fail(ctx, MOGBUS_ERR_NOMEM, "out of memory allocating scratch arena");
	if (!config_load(ctx->arena, scratch, path, &loaded))
	{
		fail(ctx, MOGBUS_ERR_CONFIG, "%s", loaded.error);
		goto done;
	}
	if (ctx->device_count + loaded.device_count > SERVER_MAX_DEVICES)
	{
		fail(ctx, MOGBUS_ERR_CONFIG, "too many devices (max %d per context)", SERVER_MAX_DEVICES);
		goto done;
	}
	for (u32 i = 0; i < loaded.device_count; i++)
	{
		if (device_conflict(ctx, loaded.devices[i]))
		{
			fail(ctx, MOGBUS_ERR_CONFIG, "unit id %u is already used by another device",
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
			if (!ctx->devices[j]->any_unit ||
			    !device_shares_server(ctx->devices[j], loaded.devices[i]))
				continue;
			fail(ctx, MOGBUS_ERR_CONFIG,
			     "devices '%s' and '%s' both set any-unit; only one device may answer every unit id",
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
	error = MOGBUS_OK;

done:
	/* Give back what a rejected load took from the context arena. */
	if (error != MOGBUS_OK)
		arena_pop_to(ctx->arena, pos);
	arena_release(scratch);
	return error;
}
#else
mogbus_result_t
mogbus_load_config(mogbus_t* ctx, const char* path)
{
	LOCK(ctx);
	(void)path;
	return fail(ctx, MOGBUS_ERR_UNSUPPORTED, "built without config support");
}
#endif

mogbus_result_t
mogbus_add_device(mogbus_t* ctx, uint8_t unit_id, const char* name, mogbus_device_t** out)
{
	device_t*	dev = NULL;
	LOCK(ctx);

	*out = NULL;
	if (ctx->device_count >= SERVER_MAX_DEVICES)
		return fail(ctx, MOGBUS_ERR_INVALID, "too many devices (max %d per context)", SERVER_MAX_DEVICES);
	if (name &&
	    strlen(name) >= DEVICE_NAME_MAX)
		return fail(ctx, MOGBUS_ERR_INVALID, "device name is too long (max %d bytes)", DEVICE_NAME_MAX - 1);
	dev = device_new(ctx->arena, os_now_ms());
	if (!dev)
		return fail(ctx, MOGBUS_ERR_NOMEM, "out of memory while creating a device");
	dev->unit_id = unit_id;
	dev->owner   = ctx;
	strcpy(dev->name, name ? name : "unnamed");
	ctx->devices[ctx->device_count++] = dev;
	*out = (mogbus_device_t*)dev;
	return MOGBUS_OK;
}

mogbus_device_t*
mogbus_find_device(mogbus_t* ctx, uint8_t unit_id)
{
	LOCK(ctx);
	return (mogbus_device_t*)find_device(ctx, unit_id);
}

mogbus_result_t
mogbus_device_add_server(mogbus_device_t* handle, const char* id)
{
	device_t*	dev   = (device_t*)handle;
	mogbus_t*	ctx   = dev->owner;
	u32		count;
	const char*	error;
	LOCK(owner_of(handle));

	count = dev->server_count;
	error = device_add_server(dev, id);
	if (error)
		return fail(ctx, MOGBUS_ERR_INVALID, "%s", error);
	if (dev->server_count != count)
	{
		/* Another device with this unit id may already be served by `id`. */
		device_t* other = device_conflict(ctx, dev);

		if (other)
		{
			dev->server_count = count;
			return fail(ctx, MOGBUS_ERR_INVALID,
				    "unit id %u of device '%s' is already used by device '%s' on server '%s'",
				    dev->unit_id, dev->name, other->name, id);
		}
	}
	return MOGBUS_OK;
}

const char*
mogbus_device_server_at(const mogbus_device_t* handle, size_t index)
{
	const device_t* dev = (const device_t*)handle;
	LOCK(owner_of(handle));

	return index < dev->server_count ? dev->servers[index] : NULL;
}

size_t
mogbus_device_count(const mogbus_t* ctx)
{
	LOCK(ctx);
	return ctx->device_count;
}

mogbus_device_t*
mogbus_device_at(mogbus_t* ctx, size_t index)
{
	LOCK(ctx);
	return index < ctx->device_count ? (mogbus_device_t*)ctx->devices[index] : NULL;
}

bool
mogbus_device_info(const mogbus_device_t* handle, mogbus_device_info_t* out)
{
	const device_t*	dev = (const device_t*)handle;
	LOCK(owner_of(handle));

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

bool
mogbus_device_point_at(const mogbus_device_t* handle, uint32_t index, mogbus_point_t* out)
{
	const device_t*	dev = (const device_t*)handle;
	const point_t*	point;
	LOCK(owner_of(handle));

	if (!dev || !out || index >= dev->point_count)
		return false;
	point = &dev->points[index];
	memset(out, 0, sizeof(*out));
	out->space       = (mogbus_space_t)point->space;
	out->address     = point->address;
	out->type        = (mogbus_value_type_t)point->type;
	out->word_order  = (mogbus_word_order_t)point->order;
	out->byte_order  = (mogbus_byte_order_t)point->byte_order;
	out->kind        = point->kind == POINT_RAMP    ? MOGBUS_POINT_RAMP :
			   point->kind == POINT_CLOCK   ? MOGBUS_POINT_CLOCK :
			   point->kind == POINT_MODEL   ? MOGBUS_POINT_MODEL :
			   point->kind == POINT_MACHINE ? MOGBUS_POINT_MACHINE : MOGBUS_POINT_STATIC;
	out->length      = point->length;
	out->read_only   = point->read_only;
	out->clock_field = (mogbus_clock_field_t)point->clock_field;
	out->clock_utc   = point->clock_utc;
	out->note        = point->note;
	return true;
}

uint64_t
mogbus_now_ms(void)
{
	return os_now_ms();
}

void
mogbus_rtu_timing(uint32_t baud, uint32_t frame_gap_ms, uint32_t* t35_us, uint32_t* gap_ms)
{
	if (t35_us)
		*t35_us = rtu_t35_us(baud);
	if (gap_ms)
		*gap_ms = rtu_effective_gap_ms(frame_gap_ms, baud);
}

mogbus_result_t
mogbus_device_add_point(mogbus_device_t* handle, const mogbus_point_t* in)
{
	device_t*	dev   = (device_t*)handle;
	mogbus_t*	ctx   = dev ? dev->owner : NULL;
	point_t		point = { 0 };
	const char*	error = NULL;
	LOCK(owner_of(handle));

	if (!dev ||
	    !in)
		return fail(ctx, MOGBUS_ERR_INVALID, "device and point are required");
	if ((unsigned)in->space >= SPACE_COUNT ||
	    (unsigned)in->type > MOGBUS_TYPE_STRING ||
	    (unsigned)in->word_order > MOGBUS_WORDS_LOW_FIRST ||
	    (unsigned)in->byte_order > MOGBUS_BYTES_LITTLE ||
	    (unsigned)in->kind > MOGBUS_POINT_CLOCK ||
	    (unsigned)in->clock_field > MOGBUS_CLOCK_UNIX ||
	    (unsigned)in->ramp.mode > MOGBUS_RAMP_HOLD)
	{
		return fail(ctx, MOGBUS_ERR_INVALID,
			    "point has an invalid space, type, word order, byte order, kind, clock field or ramp mode");
	}
	point.space   = (space_kind_t)in->space;
	point.address = in->address;
	point.type    = (value_type_t)in->type;
	point.order   = (word_order_t)in->word_order;
	point.byte_order = (byte_order_t)in->byte_order;
	point.read_only  = in->read_only;
	point.note       = in->note;
	if (point.type == VT_STRING)
	{
		if (in->length > DEVICE_MAX_STRING_REGISTERS)
		{
			return fail(ctx, MOGBUS_ERR_INVALID, "string length must be between 1 and 123 registers");
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
			return fail(ctx, MOGBUS_ERR_INVALID, "coils and discrete inputs can only be static");
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
			return fail(ctx, MOGBUS_ERR_INVALID,
				    "only float32 and float64 static values can be NaN or infinity");
		}
		point.static_value = in->value;
	}

	error = device_add_point(dev, &point);
	return error ? fail(ctx, MOGBUS_ERR_INVALID, "%s", error) : MOGBUS_OK;
}

mogbus_result_t
mogbus_device_read(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
		   uint32_t count, uint16_t* out)
{
	device_t*	dev = (device_t*)handle;
	LOCK(owner_of(handle));

	if ((unsigned)space >= SPACE_COUNT ||
	    count == 0 ||
	    !out)
		return fail(owner_of(dev), MOGBUS_ERR_INVALID, "invalid table, count or output buffer");
	device_tick(dev, os_now_ms());
	if (!device_read(dev, (space_kind_t)space, address, count, out))
		return fail(owner_of(dev), MOGBUS_ERR_INVALID, "an address in %u..%u is undefined or out of range",
			    address, address + count - 1);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_write(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
		    uint32_t count, const uint16_t* in)
{
	device_t*	dev = (device_t*)handle;
	LOCK(owner_of(handle));

	if ((unsigned)space >= SPACE_COUNT ||
	    count == 0 ||
	    !in)
		return fail(owner_of(dev), MOGBUS_ERR_INVALID, "invalid table, count or input buffer");
	if (space_is_bit((space_kind_t)space))
	{
		for (u32 i = 0; i < count; i++)
		{
			if (in[i] > 1)
				return fail(owner_of(dev), MOGBUS_ERR_INVALID, "bit tables accept only 0 or 1");
		}
	}
	device_tick(dev, os_now_ms());
	if (!device_write(dev, (space_kind_t)space, address, count, in))
		return fail(owner_of(dev), MOGBUS_ERR_INVALID,
			    "an address in %u..%u is undefined, out of range or driven by a machine or the host",
			    address, address + count - 1);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_get_value(mogbus_device_t* handle, mogbus_space_t space, uint32_t address, double* out)
{
	device_t*	dev = (device_t*)handle;
	val_t		val;
	LOCK(owner_of(handle));

	if ((unsigned)space >= SPACE_COUNT ||
	    !out)
		return fail(owner_of(dev), MOGBUS_ERR_INVALID, "invalid table or output pointer");
	device_tick(dev, os_now_ms());
	if (!device_get_point(dev, (space_kind_t)space, address, &val))
		return fail(owner_of(dev), MOGBUS_ERR_INVALID, "no numeric point starts at address %u", address);
	*out = val.type == VAL_BOOL ? (val.b ? 1.0 : 0.0) : val.n;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_set_value(mogbus_device_t* handle, mogbus_space_t space, uint32_t address, double value)
{
	device_t*	dev = (device_t*)handle;
	val_t		val = { 0 };
	LOCK(owner_of(handle));

	if ((unsigned)space >= SPACE_COUNT)
		return fail(owner_of(dev), MOGBUS_ERR_INVALID, "invalid table");
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
	if (!device_set_point(dev, (space_kind_t)space, address, &val))
		return fail(owner_of(dev), MOGBUS_ERR_INVALID, "no numeric point starts at address %u", address);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_get_string(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
			 char* out, size_t size)
{
	LOCK(owner_of(handle));
	if ((unsigned)space >= SPACE_COUNT)
		return fail(owner_of(handle), MOGBUS_ERR_INVALID, "invalid table");
	if (!device_get_string((device_t*)handle, (space_kind_t)space, address, out, size))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID,
			    "no string point starts at address %u, or the buffer is too small", address);
	return MOGBUS_OK;
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
		.target        = (mogbus_access_target_t)in->target,
		.file          = in->file_no,
		.node          = in->node_id,
		.subindex      = in->subindex,
		.index         = in->index,
		.data          = in->data,
		.data_len      = in->data_len,
	};

	return hook->fn(hook->user, (mogbus_device_t*)dev, &access);
}

/** @brief The host's post-write hook and user pointer of one device, the `user` of its trampoline. */
struct written_hook_t
{
	mogbus_written_fn fn;
	void*		user;
};

static void
written_trampoline(void* user, device_t* dev, const device_access_t* in)
{
	const struct written_hook_t*	hook   = user;
	mogbus_access_t			access = {
		.kind          = MOGBUS_ACCESS_WRITE,
		.space         = (mogbus_space_t)in->space,
		.address       = in->address,
		.count         = in->count,
		.values        = in->values,
		.function_code = in->function_code,
		.target        = (mogbus_access_target_t)in->target,
		.file          = in->file_no,
	};

	hook->fn(hook->user, (mogbus_device_t*)dev, &access);
}

mogbus_result_t
mogbus_device_add_file_record(mogbus_device_t* handle, uint16_t file, uint16_t record, uint16_t count,
			      const uint16_t* values)
{
	mogbus_t*	ctx   = handle ? ((device_t*)handle)->owner : NULL;
	const char*	error = NULL;
	LOCK(owner_of(handle));

	if (!handle ||
	    !values)
		return fail(ctx, MOGBUS_ERR_INVALID, "device and values are required");
	error = device_add_file_record((device_t*)handle, file, record, count, values);
	return error ? fail(ctx, MOGBUS_ERR_INVALID, "%s", error) : MOGBUS_OK;
}

mogbus_result_t
mogbus_device_read_file_records(mogbus_device_t* handle, uint16_t file, uint16_t record, uint16_t count,
				uint16_t* out)
{
	LOCK(owner_of(handle));
	if (!handle ||
	    !out)
		return fail(owner_of(handle), MOGBUS_ERR_INVALID, "device and output buffer are required");
	if (!device_read_file_records((device_t*)handle, file, record, count, out))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID,
			    "file %u records %u..%u are not all defined", file, record, record + count - 1);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_write_file_records(mogbus_device_t* handle, uint16_t file, uint16_t record, uint16_t count,
				 const uint16_t* values)
{
	LOCK(owner_of(handle));
	if (!handle ||
	    !values)
		return fail(owner_of(handle), MOGBUS_ERR_INVALID, "device and values are required");
	if (!device_write_file_records((device_t*)handle, file, record, count, values))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID,
			    "file %u records %u..%u are not all defined", file, record, record + count - 1);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_add_fifo(mogbus_device_t* handle, uint16_t address, uint16_t count, const uint16_t* values)
{
	mogbus_t*	ctx   = handle ? ((device_t*)handle)->owner : NULL;
	const char*	error = NULL;
	LOCK(owner_of(handle));

	if (!handle ||
	    (count > 0 && !values))
		return fail(ctx, MOGBUS_ERR_INVALID, "device and values are required");
	error = device_add_fifo((device_t*)handle, address, count, values);
	return error ? fail(ctx, MOGBUS_ERR_INVALID, "%s", error) : MOGBUS_OK;
}

mogbus_result_t
mogbus_device_set_fifo(mogbus_device_t* handle, uint16_t address, uint16_t count, const uint16_t* values)
{
	LOCK(owner_of(handle));
	if (!handle ||
	    (count > 0 && !values))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID, "device and values are required");
	if (!device_set_fifo((device_t*)handle, address, count, values))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID,
			    "no FIFO is defined at address %u, or it cannot hold %u values", address, count);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_add_canopen_object(mogbus_device_t* handle, uint8_t node, uint16_t index, uint8_t subindex,
				 uint16_t length, const uint8_t* data)
{
	mogbus_t*	ctx   = handle ? ((device_t*)handle)->owner : NULL;
	const char*	error = NULL;
	LOCK(owner_of(handle));

	if (!handle ||
	    !data)
		return fail(ctx, MOGBUS_ERR_INVALID, "device and data are required");
	error = device_add_canopen_object((device_t*)handle, node, index, subindex, length, data);
	return error ? fail(ctx, MOGBUS_ERR_INVALID, "%s", error) : MOGBUS_OK;
}

mogbus_result_t
mogbus_device_read_canopen(mogbus_device_t* handle, uint8_t node, uint16_t index, uint8_t subindex,
			   uint16_t offset, uint16_t count, uint8_t* out)
{
	LOCK(owner_of(handle));
	if (!handle ||
	    !out)
		return fail(owner_of(handle), MOGBUS_ERR_INVALID, "device and output buffer are required");
	if (!device_canopen_read((device_t*)handle, node, index, subindex, offset, count, out))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID,
			    "CANopen object %u:%04X:%u does not exist or the range is outside its length",
			    node, index, subindex);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_write_canopen(mogbus_device_t* handle, uint8_t node, uint16_t index, uint8_t subindex,
			    uint16_t offset, uint16_t count, const uint8_t* data)
{
	LOCK(owner_of(handle));
	if (!handle ||
	    !data)
		return fail(owner_of(handle), MOGBUS_ERR_INVALID, "device and data are required");
	if (!device_canopen_write((device_t*)handle, node, index, subindex, offset, count, data))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID,
			    "CANopen object %u:%04X:%u does not exist or the range is outside its length",
			    node, index, subindex);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_set_any_unit(mogbus_device_t* handle, bool on)
{
	device_t*	dev = (device_t*)handle;
	mogbus_t*	ctx = owner_of(dev);
	LOCK(owner_of(handle));

	if (!ctx)
		return fail(ctx, MOGBUS_ERR_INVALID, "a device is required");
	for (u32 i = 0; on && i < ctx->device_count; i++)
	{
		if (ctx->devices[i] != dev &&
		    ctx->devices[i]->any_unit)
			return fail(ctx, MOGBUS_ERR_INVALID, "device '%s' already answers every unit id",
				    ctx->devices[i]->name);
	}
	dev->any_unit = on;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_set_access_hook(mogbus_device_t* handle, mogbus_access_fn fn, void* user)
{
	device_t*		dev  = (device_t*)handle;
	struct access_hook_t*	hook = NULL;
	LOCK(owner_of(handle));

	if (!dev)
		return fail(NULL, MOGBUS_ERR_INVALID, "a device is required");
	if (!fn)
	{
		dev->access_fn   = NULL;
		dev->access_user = NULL;
		return MOGBUS_OK;
	}
	/* The holder is allocated once per device and reused by later calls. */
	hook = dev->access_fn == access_trampoline ? dev->access_user : ARENA_PUSH_ARRAY_ALIGNED(dev->arena, struct access_hook_t, 1, _Alignof(struct access_hook_t));
	if (!hook)
		return fail(owner_of(dev), MOGBUS_ERR_NOMEM, "out of memory while installing the hook");
	hook->fn         = fn;
	hook->user       = user;
	dev->access_fn   = access_trampoline;
	dev->access_user = hook;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_set_written_hook(mogbus_device_t* handle, mogbus_written_fn fn, void* user)
{
	device_t*		dev  = (device_t*)handle;
	struct written_hook_t*	hook = NULL;
	LOCK(owner_of(handle));

	if (!dev)
		return fail(NULL, MOGBUS_ERR_INVALID, "a device is required");
	if (!fn)
	{
		dev->written_fn   = NULL;
		dev->written_user = NULL;
		return MOGBUS_OK;
	}
	hook = dev->written_fn == written_trampoline ? dev->written_user : ARENA_PUSH_ARRAY_ALIGNED(dev->arena, struct written_hook_t, 1, _Alignof(struct written_hook_t));
	if (!hook)
		return fail(owner_of(dev), MOGBUS_ERR_NOMEM, "out of memory while installing the hook");
	hook->fn          = fn;
	hook->user        = user;
	dev->written_fn   = written_trampoline;
	dev->written_user = hook;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_device_set_string(mogbus_device_t* handle, mogbus_space_t space, uint32_t address,
			 const char* text)
{
	LOCK(owner_of(handle));
	if ((unsigned)space >= SPACE_COUNT)
		return fail(owner_of(handle), MOGBUS_ERR_INVALID, "invalid table");
	if (!device_set_string((device_t*)handle, (space_kind_t)space, address, text))
		return fail(owner_of(handle), MOGBUS_ERR_INVALID,
			    "no string point starts at address %u, or the text is too long", address);
	return MOGBUS_OK;
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
copy_string(mogbus_t* ctx, char* dst, size_t cap, const char* src, const char* what, bool* ok)
{
	if (!src)
		return NULL;
	if (strlen(src) >= cap)
	{
		fail(ctx, MOGBUS_ERR_INVALID, "%s is too long (max %zu bytes)", what, cap - 1);
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
static mogbus_result_t
validate_options(mogbus_t* ctx, server_options_t* o, struct strings_t* st, device_t** devices, u32* count)
{
	bool ok = true;

	*count = 0;
	for (u32 i = 0; i < ctx->device_count; i++)
	{
		if (device_serves(ctx->devices[i], o->id))
			devices[(*count)++] = ctx->devices[i];
	}
	if (ctx->device_count == 0)
		return fail(ctx, MOGBUS_ERR_INVALID, "the context has no devices to serve");
	if (*count == 0 &&
	    o->id)
		return fail(ctx, MOGBUS_ERR_INVALID,
			    "no device is assigned to server '%s'; see mogbus_device_add_server()", o->id);
	if (*count == 0)
		return fail(ctx, MOGBUS_ERR_INVALID,
			    "every device is assigned to a named server; a server without id serves the devices that list none");
	for (u32 i = 0; i < *count; i++)
	{
		for (u32 j = 0; j < i; j++)
		{
			if (devices[j]->unit_id == devices[i]->unit_id)
				return fail(ctx, MOGBUS_ERR_INVALID,
					    "unit id %u is used by devices '%s' and '%s' of the same server; "
					    "assign them to different servers with mogbus_device_add_server()",
					    devices[i]->unit_id, devices[j]->name, devices[i]->name);
		}
	}
	if (o->id &&
	    !o->id[0])
		return fail(ctx, MOGBUS_ERR_INVALID, "server id must not be empty");
	for (u32 i = 0; o->id && i < MOGBUS_MAX_SERVERS; i++)
	{
		if (ctx->servers[i] &&
		    ctx->servers[i]->in_use &&
		    strcmp(ctx->servers[i]->strings.id, o->id) == 0)
			return fail(ctx, MOGBUS_ERR_INVALID, "a server with id '%s' already exists", o->id);
	}
	o->id = copy_string(ctx, st->id, sizeof(st->id), o->id, "server id", &ok);
	if ((o->mode == SERVER_MODE_RTU_DEVICE || o->mode == SERVER_MODE_ASCII_DEVICE) &&
	    (!o->path || !o->path[0]))
	{
		return fail(ctx, MOGBUS_ERR_INVALID, "this mode needs a device path");
	}
	if ((o->mode == SERVER_MODE_RTU_FD || o->mode == SERVER_MODE_ASCII_FD) && o->fd < 0)
	{
		return fail(ctx, MOGBUS_ERR_INVALID, "this mode needs an open fd");
	}
	if (o->show_registers_ms != 0 &&
	    (o->show_registers_ms < 10 || o->show_registers_ms > 60000))
	{
		return fail(ctx, MOGBUS_ERR_INVALID, "show_registers_ms must be 0 or 10 to 60000");
	}
	if (o->char_timing &&
	    o->mode != SERVER_MODE_RTU_PTY &&
	    o->mode != SERVER_MODE_RTU_DEVICE &&
	    o->mode != SERVER_MODE_RTU_FD)
		return fail(ctx, MOGBUS_ERR_INVALID, "char_timing is only valid in the RTU serial modes");
	if (o->char_timing &&
	    o->baud == 0 &&
	    o->mode != SERVER_MODE_RTU_DEVICE)
		return fail(ctx, MOGBUS_ERR_INVALID, "char_timing needs a baud rate");
	if ((o->data_bits != 7 && o->data_bits != 8) ||
	    (o->stop_bits != 1 && o->stop_bits != 2) ||
	    (unsigned)o->parity > PARITY_SPACE)
	{
		return fail(ctx, MOGBUS_ERR_INVALID,
			    "serial line needs 7 or 8 data bits, 1 or 2 stop bits and a valid parity");
	}

	o->bind_address = copy_string(ctx, st->bind, sizeof(st->bind), o->bind_address, "bind address", &ok);
	o->http_bind_address = copy_string(ctx, st->http_bind, sizeof(st->http_bind), o->http_bind_address,
					   "HTTP bind address", &ok);
	/* Names are resolved here so a typo fails with its own message; the transport resolves again to bind. */
	for (u32 i = 0; i < 2 && ok; i++)
	{
		const char*	host = i == 0 ? o->bind_address : o->http_bind_address;
		os_addr_t	addr;

		if (host &&
		    i == 0 &&
		    (o->mode == SERVER_MODE_RTU_PTY ||
		     o->mode == SERVER_MODE_RTU_DEVICE ||
		     o->mode == SERVER_MODE_RTU_FD ||
		     o->mode == SERVER_MODE_ASCII_PTY ||
		     o->mode == SERVER_MODE_ASCII_DEVICE ||
		     o->mode == SERVER_MODE_ASCII_FD))
			continue;
		if (host &&
		    !os_ip_is_literal(host) &&
		    (!os_host_name_valid(host) ||
		     os_resolve(host, 0, false, &addr, 1) == 0))
		{
			return fail(ctx, MOGBUS_ERR_INVALID, "%sbind address '%s' is not an IP address and does not resolve to one",
				    i == 0 ? "" : "HTTP ", host);
		}
	}
	if (o->http_allowed_hosts && !net_obs_hosts_valid(o->http_allowed_hosts))
	{
		return fail(ctx, MOGBUS_ERR_INVALID, "HTTP allowed hosts must be at most %d comma-separated host names "
			    "of letters, digits, '.', '-' and '_' (%d characters in all)",
			    NET_OBS_HOSTS_COUNT, NET_OBS_HOSTS_MAX - 1);
	}
	o->http_allowed_hosts = copy_string(ctx, st->http_hosts, sizeof(st->http_hosts), o->http_allowed_hosts,
					    "HTTP allowed hosts", &ok);
	o->path         = copy_string(ctx, st->path, sizeof(st->path), o->path, "device path", &ok);
	o->tls.certificate = copy_string(ctx, st->certificate, sizeof(st->certificate), o->tls.certificate,
					 "TLS certificate path", &ok);
	o->tls.private_key = copy_string(ctx, st->private_key, sizeof(st->private_key), o->tls.private_key,
					 "TLS private key path", &ok);
	o->tls.ca_certificate = copy_string(ctx, st->ca_certificate, sizeof(st->ca_certificate),
					    o->tls.ca_certificate, "TLS CA path", &ok);
	o->tls.tls1_2_ciphers = copy_string(ctx, st->tls1_2_ciphers, sizeof(st->tls1_2_ciphers),
					    o->tls.tls1_2_ciphers, "TLS 1.2 cipher list", &ok);
	o->tls.tls1_3_ciphersuites = copy_string(ctx, st->tls1_3_ciphersuites, sizeof(st->tls1_3_ciphersuites),
						 o->tls.tls1_3_ciphersuites, "TLS 1.3 ciphersuites", &ok);
	if (!ok)
		return MOGBUS_ERR_INVALID;
	if (o->tls_enabled)
	{
		net_tls_ctx_t	tls;

		if (!server_check_tls(o, &tls))
			return fail(ctx, MOGBUS_ERR_LISTEN, "%s", tls.error);
	}
	return MOGBUS_OK;
}

/** @brief Common tail of both create functions; the failure message is left in the context. */
static mogbus_result_t
create_server(mogbus_t* ctx, server_options_t o, mogbus_server_t** out)
{
	mogbus_server_t*	s     = NULL;
	mogbus_result_t		error = MOGBUS_OK;
	s32			rc    = 0;

	*out = NULL;
	for (u32 i = 0; i < MOGBUS_MAX_SERVERS && !s; i++)
	{
		if (!ctx->servers[i])
			ctx->servers[i] = ARENA_PUSH_ARRAY_ALIGNED(ctx->arena, mogbus_server_t, 1, 16);
		if (ctx->servers[i] && !ctx->servers[i]->in_use)
			s = ctx->servers[i];
		else if (!ctx->servers[i])
			return fail(ctx, MOGBUS_ERR_NOMEM, "out of memory while creating a server");
	}
	if (!s)
		return fail(ctx, MOGBUS_ERR_INVALID, "too many servers (max %d per context)", MOGBUS_MAX_SERVERS);

	error = validate_options(ctx, &o, &s->strings, s->devices, &s->device_count);
	if (error != MOGBUS_OK)
	{
		/* A server that fails on TLS has always logged it; keep that for log readers. */
		if (error == MOGBUS_ERR_LISTEN)
			LOG_WARN("cannot configure TLS: %s", ctx->error);
		return error;
	}

	if (o.show_registers_ms &&
	    !view_open(&ctx->view, ctx->arena, o.show_registers_ms))
		return fail(ctx, MOGBUS_ERR_NOMEM, "out of memory opening the register view");

	o.devices      = s->devices;
	o.device_count = s->device_count;
	rc = server_init(&s->server, &o);
	if (rc != 0)
	{
		if (rc == 1)
			return fail(ctx, MOGBUS_ERR_SERIAL, "cannot open the serial device or PTY");
		return fail(ctx, MOGBUS_ERR_LISTEN,
			    "cannot listen on %s:%u (address in use, invalid, or TLS setup failed)",
			    o.bind_address ? o.bind_address : "", o.port);
	}
	s->ctx        = ctx;
	s->server.obs.lock = &ctx->lock;
	s->in_use     = true;
	s->frame_fn   = NULL;
	s->frame_user = NULL;
	s->conn_fn    = NULL;
	s->conn_user  = NULL;
	*out = s;
	return MOGBUS_OK;
}

/** @brief Public options to internal ones; false and a failure in ctx for an enum out of range. */
static bool
options_from_public(mogbus_t* ctx, const mogbus_server_options_t* in, server_options_t* o)
{
	if (!in ||
	    (unsigned)in->mode > MOGBUS_MODE_RTU_UDP ||
	    (unsigned)in->parity > MOGBUS_PARITY_SPACE ||
	    (unsigned)in->tls_min_version > MOGBUS_TLS_1_3)
	{
		fail(ctx, MOGBUS_ERR_INVALID, "invalid mode, parity or TLS version");
		return false;
	}
	*o = (server_options_t){
		.id                       = in->id,
		.show_registers_ms        = in->show_registers_ms,
		.mode                     = (server_mode_t)in->mode,
		.bind_address             = in->bind_address ? in->bind_address : "127.0.0.1",
		.http_bind_address        = in->http_bind_address,
		.http_port                = in->http_port,
		.http_control             = in->http_control,
		.http_ui                  = in->http_ui,
		.http_allowed_hosts       = in->http_allowed_hosts,
		.port                     = in->port,
		.path                     = in->path,
		.fd                       = in->fd,
		.seed                     = in->seed,
		.idle_timeout_ms          = in->idle_timeout_ms,
		.frame_gap_ms             = in->frame_gap_ms,
		.baud                     = in->baud,
		.strict_t15               = in->strict_t15,
		.char_timing              = in->char_timing,
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

mogbus_result_t
mogbus_server_create(mogbus_t* ctx, const mogbus_server_options_t* in, mogbus_server_t** out)
{
	server_options_t o;
	LOCK(ctx);

	*out = NULL;
	if (!options_from_public(ctx, in, &o))
		return MOGBUS_ERR_INVALID;
	return create_server(ctx, o, out);
}

mogbus_result_t
mogbus_server_check(mogbus_t* ctx, const mogbus_server_options_t* in)
{
	server_options_t	o;
	struct strings_t	strings;
	device_t*		devices[SERVER_MAX_DEVICES];
	u32			count;
	LOCK(ctx);

	if (!options_from_public(ctx, in, &o))
		return MOGBUS_ERR_INVALID;
	return validate_options(ctx, &o, &strings, devices, &count);
}

#if WITH_CONFIG
size_t
mogbus_config_server_count(const mogbus_t* ctx)
{
	LOCK(ctx);
	return ctx->has_config ? ctx->config.server_count : 0;
}

mogbus_result_t
mogbus_server_create_from_config_at(mogbus_t* ctx, size_t index, mogbus_server_t** out)
{
	LOCK(ctx);
	*out = NULL;
	if (!ctx->has_config)
		return fail(ctx, MOGBUS_ERR_INVALID, "no config loaded; call mogbus_load_config first");
	if (index >= ctx->config.server_count)
		return fail(ctx, MOGBUS_ERR_INVALID, "the config has %u server%s, no server %zu",
			    ctx->config.server_count, ctx->config.server_count == 1 ? "" : "s", index);
	return create_server(ctx, server_options_from_config(&ctx->config, (u32)index), out);
}

mogbus_result_t
mogbus_server_create_from_config(mogbus_t* ctx, mogbus_server_t** out)
{
	LOCK(ctx);
	return mogbus_server_create_from_config_at(ctx, 0, out);
}

mogbus_result_t
mogbus_config_server_options_at(mogbus_t* ctx, size_t index, mogbus_server_options_t* out)
{
	server_options_t	o;
	LOCK(ctx);

	if (!ctx->has_config)
		return fail(ctx, MOGBUS_ERR_INVALID, "no config loaded; call mogbus_load_config first");
	if (index >= ctx->config.server_count)
		return fail(ctx, MOGBUS_ERR_INVALID, "the config has %u server%s, no server %zu",
			    ctx->config.server_count, ctx->config.server_count == 1 ? "" : "s", index);
	o = server_options_from_config(&ctx->config, (u32)index);
	*out = (mogbus_server_options_t){
		.id                             = o.id,
		.show_registers_ms              = o.show_registers_ms,
		.mode                           = (mogbus_mode_t)o.mode,
		.bind_address                   = o.bind_address,
		.port                           = o.port,
		.http_bind_address              = o.http_bind_address,
		.http_port                      = o.http_port,
		.http_control                   = o.http_control,
		.http_ui                        = o.http_ui,
		.http_allowed_hosts             = o.http_allowed_hosts,
		.path                           = o.path[0] ? o.path : NULL,
		.fd                             = -1,
		.seed                           = o.seed,
		.idle_timeout_ms                = o.idle_timeout_ms,
		.frame_gap_ms                   = o.frame_gap_ms,
		.baud                           = o.baud,
		.strict_t15                     = o.strict_t15,
		.char_timing                    = o.char_timing,
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
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_config_server_options(mogbus_t* ctx, mogbus_server_options_t* out)
{
	LOCK(ctx);
	return mogbus_config_server_options_at(ctx, 0, out);
}
#else
size_t
mogbus_config_server_count(const mogbus_t* ctx)
{
	LOCK(ctx);
	(void)ctx;
	return 0;
}

mogbus_result_t
mogbus_server_create_from_config_at(mogbus_t* ctx, size_t index, mogbus_server_t** out)
{
	LOCK(ctx);
	(void)index;
	*out = NULL;
	return fail(ctx, MOGBUS_ERR_UNSUPPORTED, "built without config support");
}

mogbus_result_t
mogbus_server_create_from_config(mogbus_t* ctx, mogbus_server_t** out)
{
	LOCK(ctx);
	return mogbus_server_create_from_config_at(ctx, 0, out);
}

mogbus_result_t
mogbus_config_server_options_at(mogbus_t* ctx, size_t index, mogbus_server_options_t* out)
{
	LOCK(ctx);
	(void)index;
	(void)out;
	return fail(ctx, MOGBUS_ERR_UNSUPPORTED, "built without config support");
}

mogbus_result_t
mogbus_config_server_options(mogbus_t* ctx, mogbus_server_options_t* out)
{
	LOCK(ctx);
	return mogbus_config_server_options_at(ctx, 0, out);
}
#endif

int
mogbus_server_step(mogbus_server_t* server, int max_wait_ms)
{
	LOCK(server ? server->ctx : NULL);
	return server_step(&server->server, max_wait_ms);
}

/**
 * @brief Runs the gather phase of a group step for `servers` and copies the wait table out.
 *
 * Returns MOGBUS_ERR_INVALID (nothing copied) if `cap` is too small. The timeout is 0 when a
 * gathered step already reports done/error (stop requested), so the host steps at once.
 */
static mogbus_result_t
poll_fds_of(mogbus_t* ctx, server_t* const* servers, u32 n, mogbus_poll_fd_t* out, size_t cap,
	    size_t* count, int* timeout_ms)
{
	net_poll_group_t*	group = &ctx->group;
	u32			live  = 0;
	bool			ready = false;

	group->phase      = NET_POLL_GATHER;
	group->count      = 0;
	group->timeout_ms = -1;
	for (u32 i = 0; i < n; i++)
	{
		server_t* s = servers[i];

		if (!s ||
		    s->done)
			continue;
		s->obs.group = group;
		if (server_step(s, -1) == NET_STEP_CONTINUE)
			live++;
		else
			ready = true;
		s->obs.group = NULL;
	}
	*count      = group->count;
	*timeout_ms = (ready || !live) ? 0 : (group->timeout_ms < 0 || group->timeout_ms > 1000) ? 1000 : group->timeout_ms;
	if (group->count > cap)
		return fail(ctx, MOGBUS_ERR_INVALID, "poll table needs %u entries, got room for %zu", group->count, cap);
	*count = 0;
	for (u32 i = 0; i < group->count; i++)
	{
		const os_wait_entry_t* e = &group->entries[i];

		if (!os_socket_valid(e->fd))
			continue;
		out[(*count)++] = (mogbus_poll_fd_t){
			.fd     = (uint64_t)e->fd,
			.events = ((e->events & OS_WAIT_IN) ? MOGBUS_POLL_IN : 0) |
				  ((e->events & OS_WAIT_OUT) ? MOGBUS_POLL_OUT : 0),
		};
	}
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_server_poll_fds(mogbus_server_t* server, mogbus_poll_fd_t* out, size_t cap, size_t* count,
		       int* timeout_ms)
{
	server_t*	one;
	LOCK(server ? server->ctx : NULL);

	if (!server ||
	    !server->in_use ||
	    !count ||
	    !timeout_ms ||
	    (!out && cap))
		return MOGBUS_ERR_INVALID;
	one = &server->server;
	return poll_fds_of(server->ctx, &one, 1, out, cap, count, timeout_ms);
}

mogbus_result_t
mogbus_poll_fds(mogbus_t* ctx, mogbus_poll_fd_t* out, size_t cap, size_t* count, int* timeout_ms)
{
	server_t*	servers[MOGBUS_MAX_SERVERS] = { NULL };
	mogbus_result_t	rc;
	LOCK(ctx);

	if (!ctx ||
	    !count ||
	    !timeout_ms ||
	    (!out && cap))
		return MOGBUS_ERR_INVALID;
	for (u32 i = 0; i < MOGBUS_MAX_SERVERS; i++)
	{
		if (ctx->servers[i] &&
		    ctx->servers[i]->in_use)
			servers[i] = &ctx->servers[i]->server;
	}
	rc = poll_fds_of(ctx, servers, MOGBUS_MAX_SERVERS, out, cap, count, timeout_ms);
	if (rc == MOGBUS_OK &&
	    ctx->view.interval_ms)
	{
		u32 due = view_wait_ms(&ctx->view, os_now_ms());

		if ((u32)*timeout_ms > due)
			*timeout_ms = (int)due;
	}
	return rc;
}

void
mogbus_server_stop(mogbus_server_t* server)
{
	server_stop(&server->server);
}

void
mogbus_stop(mogbus_t* ctx)
{
	for (u32 i = 0; i < MOGBUS_MAX_SERVERS; i++)
	{
		if (ctx->servers[i] &&
		    ctx->servers[i]->in_use)
			server_stop(&ctx->servers[i]->server);
	}
}

static mogbus_server_t* volatile* g_stop_slot;
static mogbus_t* volatile* g_stop_ctx_slot;

static void
stop_from_signal(void)
{
	mogbus_server_t*	server = g_stop_slot ? *g_stop_slot : NULL;
	mogbus_t*		ctx    = g_stop_ctx_slot ? *g_stop_ctx_slot : NULL;

	if (server)
		mogbus_server_stop(server);
	if (ctx)
		mogbus_stop(ctx);
}

void
mogbus_stop_on_signals(mogbus_server_t* volatile* slot)
{
	g_stop_slot = slot;
	os_console_on_stop(stop_from_signal);
}

void
mogbus_stop_context_on_signals(mogbus_t* volatile* slot)
{
	g_stop_ctx_slot = slot;
	os_console_on_stop(stop_from_signal);
}

int
mogbus_step(mogbus_t* ctx, int max_wait_ms)
{
	server_t*	servers[MOGBUS_MAX_SERVERS] = { NULL };
	int		rc;
	LOCK(ctx);

	for (u32 i = 0; i < MOGBUS_MAX_SERVERS; i++)
	{
		if (ctx->servers[i] &&
		    ctx->servers[i]->in_use)
			servers[i] = &ctx->servers[i]->server;
	}
	if (ctx->view.interval_ms)
	{
		u32 due = view_wait_ms(&ctx->view, os_now_ms());

		if (max_wait_ms < 0 ||
		    (u32)max_wait_ms > due)
			max_wait_ms = due > 60000 ? 60000 : (int)due;
	}
	rc = server_group_step(servers, MOGBUS_MAX_SERVERS, &ctx->group, max_wait_ms);
	view_update(&ctx->view, ctx->devices, ctx->device_count, os_now_ms());
	return rc;
}

int
mogbus_run(mogbus_t* ctx)
{
	int rc;

	/* One locked step at a time: the lock is free between steps for other threads. */
	while ((rc = mogbus_step(ctx, -1)) == MOGBUS_STEP_CONTINUE)
		;
	LOCK(ctx);
	for (u32 i = 0; i < MOGBUS_MAX_SERVERS; i++)
	{
		if (ctx->servers[i] &&
		    ctx->servers[i]->in_use)
			server_close(&ctx->servers[i]->server);
	}
	view_close(&ctx->view);
	return rc == MOGBUS_STEP_ERROR ? 1 : 0;
}

int
mogbus_server_run(mogbus_server_t* server)
{
	int rc;

	while ((rc = mogbus_server_step(server, -1)) == MOGBUS_STEP_CONTINUE)
		;
	{
		LOCK(server->ctx);
		server_close(&server->server);
	}
	return rc == MOGBUS_STEP_ERROR ? 1 : 0;
}

void
mogbus_server_close(mogbus_server_t* server)
{
	LOCK(server ? server->ctx : NULL);
	if (!server || !server->in_use)
		return;
	server_close(&server->server);
	server->in_use = false;
}

/** @brief Adapts the net-layer frame hook to the public one. */
static u32
frame_trampoline(void* user, net_frame_dir_t dir, const char* peer, u8* data, u32 len, u32 capacity)
{
	mogbus_server_t*	server = user;
	mogbus_frame_t		frame  = {
		.direction = dir == NET_FRAME_SENT ? MOGBUS_FRAME_SENT : MOGBUS_FRAME_RECEIVED,
		.mode      = (mogbus_mode_t)server->server.mode,
		.peer      = peer ? peer : "",
		.data      = data,
		.len       = len,
		.capacity  = capacity,
	};
	size_t			out    = server->frame_fn(server->frame_user, server, &frame);

	return out > capacity ? capacity + 1 : (u32)out;
}

mogbus_result_t
mogbus_server_set_frame_hook(mogbus_server_t* server, mogbus_frame_fn fn, void* user)
{
	LOCK(server ? server->ctx : NULL);
	if (!server ||
	    !server->in_use)
		return fail(server ? server->ctx : NULL, MOGBUS_ERR_INVALID, "a running server is required");
	server->frame_fn          = fn;
	server->frame_user        = user;
	server->server.obs.frame_fn   = fn ? frame_trampoline : NULL;
	server->server.obs.frame_user = server;
	return MOGBUS_OK;
}

/** @brief Adapts the net-layer connection hook to the public one. */
static void
conn_trampoline(void* user, u64 id, const char* peer, net_close_reason_t reason)
{
	mogbus_server_t*	server = user;
	mogbus_connection_t	event  = {
		.connected = reason == NET_CLOSE_NONE,
		.id        = id,
		.mode      = (mogbus_mode_t)server->server.mode,
		.peer      = peer,
		.reason    = (mogbus_close_reason_t)reason,
	};

	server->conn_fn(server->conn_user, server, &event);
}

mogbus_result_t
mogbus_server_set_connection_hook(mogbus_server_t* server, mogbus_connection_fn fn, void* user)
{
	LOCK(server ? server->ctx : NULL);
	if (!server ||
	    !server->in_use)
		return fail(server ? server->ctx : NULL, MOGBUS_ERR_INVALID, "a running server is required");
	server->conn_fn              = fn;
	server->conn_user            = user;
	server->server.obs.conn_fn   = fn ? conn_trampoline : NULL;
	server->server.obs.conn_user = server;
	return MOGBUS_OK;
}

const char*
mogbus_server_pty_name(const mogbus_server_t* server)
{
	LOCK(server ? server->ctx : NULL);
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
	LOCK(server ? server->ctx : NULL);
	if (!server || !server->in_use || !out || size == 0)
		return 0;
	return net_obs_render_metrics(&server->server.obs, out, size > UINT32_MAX ? UINT32_MAX : (u32)size);
}

/** @brief The host's callback and user pointer of one user-defined function code. */
struct function_hook_t
{
	mogbus_function_fn fn;
	void*		user;
};

static int
function_trampoline(void* user, device_t* dev, u8 unit, u8 code, const u8* data, u32 len,
		    u8* reply, u32 cap, u32* reply_len)
{
	const struct function_hook_t*	hook = user;
	size_t				out  = 0;
	int				result = hook->fn(hook->user, (mogbus_device_t*)dev, unit, code, data, len,
							  reply, cap, &out);

	*reply_len = out > cap ? cap + 1 : (u32)out;
	return result;
}

mogbus_result_t
mogbus_device_add_function_code(mogbus_device_t* handle, uint8_t code, mogbus_function_fn fn, void* user)
{
	device_t*		dev  = (device_t*)handle;
	struct function_hook_t*	hook = NULL;
	device_custom_fc_t	entry = { 0 };
	const char*		error = NULL;
	LOCK(owner_of(handle));

	if (!dev ||
	    !fn ||
	    code < 1 ||
	    code > 127 ||
	    modbus_fc_is_builtin(code))
		return fail(owner_of(dev), MOGBUS_ERR_INVALID,
			    "function code %u must be 1-127, not built in, and have a callback", code);
	hook = ARENA_PUSH_ARRAY_ALIGNED(dev->arena, struct function_hook_t, 1, _Alignof(struct function_hook_t));
	if (!hook)
		return fail(owner_of(dev), MOGBUS_ERR_NOMEM, "out of memory while adding the function code");
	hook->fn   = fn;
	hook->user = user;
	entry.code = code;
	entry.fn   = function_trampoline;
	entry.user = hook;
	error = device_add_custom_fc(dev, &entry);
	return error ? fail(owner_of(dev), MOGBUS_ERR_INVALID, "%s", error) : MOGBUS_OK;
}
