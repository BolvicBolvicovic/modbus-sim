/**
 * @file
 * @brief Runtime control API: routing, a small JSON reader and writer, and the device and
 *        fault operations behind `/api/`.
 */
#include "net/net_control.h"
#include "net/net_common.h"
#include "os/os_time.h"
#include "util/log.h"
#include "vlib/macros.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct body_t body_t;

/** @brief Limits of the runtime control request bodies. */
enum
{
	MAX_VALUES   = 512,	/**< a 1024-byte body holds at most this many numbers */
	MAX_REGISTERS = 125,
	MAX_BITS     = 2000,
	POINTS_DEFAULT_LIMIT = 100,
	POINTS_MAX_LIMIT     = 200,	/* ~125 bytes each without a note */
	POINT_ENTRY_MAX      = 960,	/* worst entry: fields plus a note escaped to 6 bytes per byte */
	ACTIONS_DEFAULT_LIMIT = 50,
	ACTIONS_MAX_LIMIT    = 256,
	ACTION_ENTRY_MAX     = 4096,	/* worst case of one listed action: 16 params with 63-char names */
};

static const char* const SPACE_NAMES[SPACE_COUNT] = {
	[SPACE_COIL]     = "coil",
	[SPACE_DISCRETE] = "discrete",
	[SPACE_INPUT]    = "input",
	[SPACE_HOLDING]  = "holding",
};

/** @brief The JSON fields a request body may carry; a body with any other key is rejected. */
enum { F_VALUES, F_VALUE, F_PERCENT, F_DELAY_MS, F_EXCEPTION_CODE, F_MODE, F_NAME, F_COUNT };

/** @brief A parsed control request body. */
struct body_t
{
	bool	has[F_COUNT];
	f64	num[F_COUNT];		/**< F_VALUE .. F_EXCEPTION_CODE */
	u32	value_count;
	f64	values[MAX_VALUES];
	char	mode[16];
	char	name[MACHINE_NAME_MAX];
};

static const char* const BODY_KEYS[F_COUNT] = {
	[F_VALUES] = "values", [F_VALUE] = "value", [F_PERCENT] = "percent",
	[F_DELAY_MS] = "delay_ms", [F_EXCEPTION_CODE] = "exception_code", [F_MODE] = "mode", [F_NAME] = "name",
};

static const char*
skip_ws(const char* p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

/* Reads a JSON number (no hex, inf or nan). Returns the end, or NULL. */
static const char*
read_number(const char* p, f64* out)
{
	char		buf[40];
	char*		end;
	u32		len = (u32)strspn(p, "0123456789+-.eE");

	if (len == 0 || len >= sizeof(buf))
		return NULL;
	memcpy(buf, p, len);
	buf[len] = '\0';
	*out = strtod(buf, &end);
	if (end != buf + len || !isfinite(*out))
		return NULL;
	return p + len;
}

/* Reads a string without escapes into `out` (truncating is an error). Returns the end, or NULL. */
static const char*
read_string(const char* p, char* out, u32 size)
{
	u32 n = 0;

	if (*p != '"')
		return NULL;
	for (p++; *p != '"'; p++)
	{
		if (*p == '\0' || *p == '\\' || (u8)*p < 0x20 || n + 1 >= size)
			return NULL;
		out[n++] = *p;
	}
	out[n] = '\0';
	return p + 1;
}

/* Parses a flat JSON object into `b`. Returns NULL, or a static error message. */
static const char*
parse_body(const char* p, body_t* b)
{
	bool first = true;

	memset(b, 0, sizeof(*b));
	p = skip_ws(p);
	if (*p != '{')
		return "body must be a JSON object";
	p = skip_ws(p + 1);
	while (*p != '}')
	{
		char	key[24];
		u32	f;

		if (!first)
		{
			if (*p != ',')
				return "malformed JSON object";
			p = skip_ws(p + 1);
		}
		first = false;
		p = read_string(p, key, sizeof(key));
		if (!p)
			return "malformed JSON key";
		for (f = 0; f < F_COUNT && strcmp(key, BODY_KEYS[f]) != 0; f++)
			;
		if (f == F_COUNT)
			return "unknown field";
		if (b->has[f])
			return "duplicate field";
		p = skip_ws(p);
		if (*p != ':')
			return "malformed JSON object";
		p = skip_ws(p + 1);
		if (f == F_MODE)
			p = read_string(p, b->mode, sizeof(b->mode));
		else if (f == F_NAME)
			p = read_string(p, b->name, sizeof(b->name));
		else if (f == F_VALUES)
		{
			if (*p != '[')
				return "values must be an array";
			p = skip_ws(p + 1);
			while (*p != ']')
			{
				if (b->value_count > 0)
				{
					if (*p != ',')
						return "malformed values array";
					p = skip_ws(p + 1);
				}
				if (b->value_count >= MAX_VALUES)
					return "too many values";
				p = read_number(p, &b->values[b->value_count++]);
				if (!p)
					return "values must be numbers";
				p = skip_ws(p);
			}
			p++;
		}
		else
			p = read_number(p, &b->num[f]);
		if (!p)
			return f == F_MODE ? "mode must be a string" :
			       f == F_NAME ? "name must be a string" : "field must be a number";
		b->has[f] = true;
		p = skip_ws(p);
	}
	if (*skip_ws(p + 1) != '\0')
		return "trailing data after the JSON object";
	return NULL;
}

/* Parses a whole non-negative decimal integer segment. */
static bool
parse_uint(const char* s, u32 len, u32 max, u32* out)
{
	u64 v = 0;

	if (len == 0 || len > 10)
		return false;
	for (u32 i = 0; i < len; i++)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (u64)(s[i] - '0');
	}
	if (v > max)
		return false;
	*out = (u32)v;
	return true;
}

static bool
whole(f64 v, f64 max)
{
	return v >= 0 && v <= max && v == floor(v);
}

/* Bounded printf into the response: `*used` past `cap` marks an overflow. */
static void __attribute__((format(printf, 4, 5)))
put(char* out, u32 cap, u32* used, const char* fmt, ...)
{
	va_list	ap;
	int	n;

	if (*used >= cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(out + *used, cap - *used, fmt, ap);
	va_end(ap);
	*used = n < 0 ? cap : *used + (u32)n;
}

/* Why a decoded value read or write was refused: string points have no numeric value. */
static const char*
value_refusal(const device_t* dev, space_kind_t space, u32 address)
{
	u32 idx = device_find_point(dev, space, address);

	if (idx < dev->point_count &&
	    dev->points[idx].type == VT_STRING)
		return "string points have no numeric value; read or write their registers";
	return "no point starts at this address";
}

static u32
fail(char* out, u32 cap, int* status, int code, const char* message)
{
	*status = code;
	return (u32)snprintf(out, cap, "{\"error\":\"%s\"}", message);
}

static u32
done(char* out, u32 cap, int* status)
{
	*status = 200;
	return (u32)snprintf(out, cap, "{\"ok\":true}");
}

static u32
list_devices(const net_obs_t* obs, char* out, u32 cap)
{
	u32 used = 0;

	put(out, cap, &used, "[");
	for (u32 i = 0; i < obs->device_count; i++)
	{
		const device_t*	dev = obs->devices[i];
		u32		points[SPACE_COUNT] = { 0 };
		char		name[2 * DEVICE_NAME_MAX + 2];

		for (u32 p = 0; p < dev->point_count; p++)
			points[dev->points[p].space]++;
		log_json_escape(name, sizeof(name), dev->name);
		put(out, cap, &used, "%s{\"unit\":%u,\"name\":\"%s\",\"points\":{", i ? "," : "",
		    dev->unit_id, name);
		for (u32 s = 0; s < SPACE_COUNT; s++)
			put(out, cap, &used, "%s\"%s\":%u", s ? "," : "", SPACE_NAMES[s], points[s]);
		put(out, cap, &used, "}}");
	}
	put(out, cap, &used, "]");
	return used;
}

/* Value of the query parameter `name` (a decimal number); `*out` keeps its default when absent. */
static bool
query_uint(const char* query, const char* name, u32 max, u32* out)
{
	u32 n = (u32)strlen(name);

	for (const char* q = query; *q;)
	{
		u32 len = (u32)strcspn(q, "&");

		if (len > n &&
		    memcmp(q, name, n) == 0 &&
		    q[n] == '=')
			return parse_uint(q + n + 1, len - n - 1, max, out);
		q += len + (q[len] == '&');
	}
	return true;
}

/*
 * One page of the points of a space, in address order. The response buffer holds a page of
 * POINTS_MAX_LIMIT entries; `next` is the first address of the following page (null at the end),
 * so a client that follows it never misses a point.
 */
static u32
list_points(const device_t* dev, space_kind_t space, u32 from, u32 limit, char* out, u32 cap,
	    int* status)
{
	static const char* const TYPES[] = {
		[VT_U16] = "u16", [VT_S16] = "s16", [VT_U32] = "u32", [VT_S32] = "s32", [VT_F32] = "float32",
		[VT_BOOL] = "bit", [VT_U64] = "u64", [VT_S64] = "s64", [VT_F64] = "float64", [VT_STRING] = "string",
	};
	static const char* const KINDS[] = {
		[POINT_STATIC] = "static", [POINT_RAMP] = "ramp", [POINT_MODEL] = "model",
		[POINT_MACHINE] = "machine", [POINT_CLOCK] = "clock",
	};
	const space_t*	sp = &dev->spaces[space];
	u32		used = 0;
	u32		n = 0;
	u32		next = ADDRESS_SPACE_SIZE;
	char		note[6 * DEVICE_NOTE_MAX];

	put(out, cap, &used, "{\"points\":[");
	for (u32 a = from; a < ADDRESS_SPACE_SIZE; a++)
	{
		const point_t* pt;

		if (sp->owner[a] == 0 ||
		    (u32)sp->owner[a] > dev->point_count)
			continue;
		pt = &dev->points[sp->owner[a] - 1];
		if (pt->space != space ||
		    pt->address != a)
			continue;
		/* An entry is at most ~160 bytes plus a note of 127 bytes escaped to 6 each (~760); stop early
		 * rather than overflow, `next` continues the listing. */
		if (n == limit ||
		    (used + POINT_ENTRY_MAX + 16 > cap))
		{
			next = a;
			break;
		}
		note[0] = '\0';
		if (pt->note)
			log_json_escape(note, sizeof(note), pt->note);
		put(out, cap, &used,
		    "%s{\"address\":%u,\"registers\":%u,\"type\":\"%s\",\"kind\":\"%s\",\"read_only\":%s,"
		    "\"word_order\":\"%s\",\"byte_order\":\"%s\"%s%s%s}",
		    n ? "," : "", a, point_width(pt), TYPES[pt->type],
		    KINDS[pt->kind], pt->read_only ? "true" : "false",
		    pt->order == WORDS_LOW_FIRST ? "low-first" : "high-first",
		    pt->byte_order == BYTES_LITTLE ? "little" : "big",
		    pt->note ? ",\"note\":\"" : "", note, pt->note ? "\"" : "");
		n++;
	}
	if (next < ADDRESS_SPACE_SIZE)
		put(out, cap, &used, "],\"next\":%u}", next);
	else
		put(out, cap, &used, "],\"next\":null}");
	return used < cap ? used : fail(out, cap, status, 500, "response too large");
}

/*
 * One page of the point actions, in the device's action order (space, address, read before
 * write). `from` is an index; a page ends at `limit` entries or when the buffer could not hold
 * another worst-case entry, and `next` is the index to continue from (null at the end).
 */
static u32
list_actions(const device_t* dev, u32 from, u32 limit, char* out, u32 cap, int* status)
{
	u32	used = 0;
	u32	n = 0;
	u32	i = from;

	put(out, cap, &used, "{\"actions\":[");
	for (const action_instance_t* a = device_action_at(dev, i); a; a = device_action_at(dev, ++i))
	{
		char name[2 * MACHINE_NAME_MAX + 2];

		if (n == limit ||
		    used + ACTION_ENTRY_MAX > cap)
			break;
		log_json_escape(name, sizeof(name), a->name);
		put(out, cap, &used,
		    "%s{\"space\":\"%s\",\"address\":%u,\"event\":\"%s\",\"name\":\"%s\",\"params\":[",
		    n ? "," : "", SPACE_NAMES[a->space], a->address,
		    a->event == ACTION_ON_WRITE ? "write" : "read", name);
		for (u32 p = 0; p < a->param_count; p++)
		{
			const val_t* v = &a->vars[p];

			log_json_escape(name, sizeof(name), a->defs[p].name);
			put(out, cap, &used, "%s{\"name\":\"%s\",\"type\":\"%s\",\"value\":", p ? "," : "",
			    name, v->type == VAL_BOOL ? "bool" : "number");
			if (v->type == VAL_BOOL)
				put(out, cap, &used, "%d}", v->b ? 1 : 0);
			else
				put(out, cap, &used, "%.15g}", v->n);
		}
		put(out, cap, &used, "]}");
		n++;
	}
	if (device_action_at(dev, i))
		put(out, cap, &used, "],\"next\":%u}", i);
	else
		put(out, cap, &used, "],\"next\":null}");
	return used < cap ? used : fail(out, cap, status, 500, "response too large");
}

static u32
list_faults(const device_t* dev, char* out, u32 cap)
{
	u32	used = 0;
	bool	first = true;

	put(out, cap, &used, "{\"faults\":[");
	for (u32 k = 0; k < FAULT_KIND_COUNT; k++)
	{
		const fault_t* f = &dev->faults.by_kind[k];

		if ((dev->faults.present & FAULT_BIT(k)) == 0)
			continue;
		put(out, cap, &used, "%s{\"kind\":\"%s\",\"percent\":%u,\"scheduled\":%s", first ? "" : ",",
		    fault_kind_name((fault_kind_t)k), f->percent, f->schedule_id ? "true" : "false");
		if (k == FAULT_DELAY)
			put(out, cap, &used, ",\"delay_ms\":%u", f->delay_ms);
		if (k == FAULT_EXCEPTION)
			put(out, cap, &used, ",\"exception_code\":%u", f->exception_code);
		if (k == FAULT_CORRUPT)
			put(out, cap, &used, ",\"mode\":\"%s\"", fault_corrupt_mode_name(f->corrupt_mode));
		put(out, cap, &used, "}");
		first = false;
	}
	put(out, cap, &used, "]}");
	return used;
}

static u32
put_fault(net_obs_t* obs, device_t* dev, fault_kind_t kind, const body_t* b, char* out, u32 cap,
	  int* status)
{
	fault_t		fault = { .percent = 100 };
	const char*	error;

	if ((b->has[F_VALUES] || b->has[F_VALUE] || b->has[F_NAME]) ||
	    (b->has[F_DELAY_MS] && kind != FAULT_DELAY) ||
	    (b->has[F_EXCEPTION_CODE] && kind != FAULT_EXCEPTION) ||
	    (b->has[F_MODE] && kind != FAULT_CORRUPT))
		return fail(out, cap, status, 400, "field does not apply to this fault kind");
	if ((kind == FAULT_DELAY && !b->has[F_DELAY_MS]) ||
	    (kind == FAULT_EXCEPTION && !b->has[F_EXCEPTION_CODE]))
		return fail(out, cap, status, 400, "required field is missing");
	if ((b->has[F_PERCENT] && !whole(b->num[F_PERCENT], 100)) ||
	    (b->has[F_DELAY_MS] && !whole(b->num[F_DELAY_MS], 1e9)) ||
	    (b->has[F_EXCEPTION_CODE] && !whole(b->num[F_EXCEPTION_CODE], 255)))
		return fail(out, cap, status, 422, "field out of range");
	if (b->has[F_PERCENT])
		fault.percent = (u32)b->num[F_PERCENT];
	if (b->has[F_DELAY_MS])
		fault.delay_ms = (u32)b->num[F_DELAY_MS];
	if (b->has[F_EXCEPTION_CODE])
		fault.exception_code = (u8)b->num[F_EXCEPTION_CODE];
	if (kind == FAULT_CORRUPT)
	{
		if (!b->has[F_MODE] || strcmp(b->mode, "pdu") == 0)
			fault.corrupt_mode = FAULT_CORRUPT_PDU;
		else if (strcmp(b->mode, "register") == 0)
			fault.corrupt_mode = FAULT_CORRUPT_REGISTER;
		else if (strcmp(b->mode, "checksum") == 0)
			fault.corrupt_mode = FAULT_CORRUPT_CHECKSUM;
		else
			return fail(out, cap, status, 400, "mode must be pdu, register or checksum");
	}
	error = fault_check(kind, &fault, obs->control.tcp_like, obs->control.mbap);
	if (error)
		return fail(out, cap, status, 422, error);
	dev->faults.by_kind[kind]       = fault;
	dev->faults.present            |= FAULT_BIT(kind);
	dev->fault_runtime.by_kind[kind] = (fault_schedule_state_t){ 0 };
	LOG_INFO("control: device %u fault %s set", dev->unit_id, fault_kind_name(kind));
	return done(out, cap, status);
}

u32
net_control_handle(net_obs_t* obs, const char* method, const char* path, const char* query,
		   const char* body, char* out, u32 cap, int* status)
{
	static body_t	parsed;		/* ~4 KB: kept off the stack; the server is single-threaded */
	const char*	seg[7];
	u32		seg_len[7];
	u32		nseg = 0;
	bool		get = strcmp(method, "GET") == 0;
	bool		put_m = strcmp(method, "PUT") == 0;
	bool		del = strcmp(method, "DELETE") == 0;
	const char*	error;
	device_t*	dev;
	u32		unit;
	u32		address;
	u32		space = SPACE_COUNT;
	u32		used = 0;
	u64		now_ms = os_now_ms();

	/* Split "/api/a/b/c" into segments; more than 6 after "api" is simply unknown. */
	for (const char* p = path + strlen("/api/"); *p && nseg < 7;)
	{
		u32 len = (u32)strcspn(p, "/");

		seg[nseg]     = p;
		seg_len[nseg] = len;
		nseg++;
		p += len;
		if (*p == '/')
			p++;
	}
	if (nseg < 1 || seg_len[0] != 7 || memcmp(seg[0], "devices", 7) != 0 || nseg > 6)
		return fail(out, cap, status, 404, "unknown path");

	*status = 200;
	if (nseg == 1)
	{
		if (!get)
			return fail(out, cap, status, 405, "method not allowed");
		used = list_devices(obs, out, cap);
		return used < cap ? used : fail(out, cap, status, 500, "response too large");
	}
	if (!parse_uint(seg[1], seg_len[1], 255, &unit))
		return fail(out, cap, status, 400, "unit must be a number from 0 to 255");
	dev = net_find_device(obs->devices, obs->device_count, (u8)unit, false);
	if (!dev || dev->unit_id != unit)
		return fail(out, cap, status, 404, "unknown unit");
	if (nseg == 2)
		return fail(out, cap, status, 404, "unknown path");

	if (seg_len[2] == 6 && memcmp(seg[2], "points", 6) == 0)
	{
		u32		from = 0;
		u32		limit = POINTS_DEFAULT_LIMIT;
		const char*	sp = strstr(query, "space=");
		u32		sp_len = sp ? (u32)strcspn(sp + 6, "&") : 0;

		if (nseg != 3)
			return fail(out, cap, status, 404, "unknown path");
		if (!get)
			return fail(out, cap, status, 405, "method not allowed");
		for (u32 s = 0; sp && (sp == query || sp[-1] == '&') && s < SPACE_COUNT; s++)
		{
			if (strlen(SPACE_NAMES[s]) == sp_len &&
			    memcmp(SPACE_NAMES[s], sp + 6, sp_len) == 0)
				space = s;
		}
		if (space == SPACE_COUNT)
			return fail(out, cap, status, 400, "space must be coil, discrete, input or holding");
		if (!query_uint(query, "from", 65535, &from) ||
		    !query_uint(query, "limit", POINTS_MAX_LIMIT, &limit) ||
		    limit == 0)
			return fail(out, cap, status, 400, "from must be 0-65535 and limit 1-200");
		return list_points(dev, (space_kind_t)space, from, limit, out, cap, status);
	}

	if (seg_len[2] == 7 && memcmp(seg[2], "actions", 7) == 0)
	{
		u32		from = 0;
		u32		limit = ACTIONS_DEFAULT_LIMIT;
		action_event_t	event = ACTION_ON_READ;
		val_t		current = { 0 };
		val_t		next = { 0 };

		if (nseg == 3)
		{
			if (!get)
				return fail(out, cap, status, 405, "method not allowed");
			if (!query_uint(query, "from", 65535, &from) ||
			    !query_uint(query, "limit", ACTIONS_MAX_LIMIT, &limit) ||
			    limit == 0)
				return fail(out, cap, status, 400, "from must be 0-65535 and limit 1-256");
			return list_actions(dev, from, limit, out, cap, status);
		}
		if (nseg != 6)
			return fail(out, cap, status, 404, "unknown path");
		if (!put_m)
			return fail(out, cap, status, 405, "method not allowed");
		for (u32 s = 0; s < SPACE_COUNT; s++)
		{
			if (strlen(SPACE_NAMES[s]) == seg_len[3] &&
			    memcmp(SPACE_NAMES[s], seg[3], seg_len[3]) == 0)
				space = s;
		}
		if (space == SPACE_COUNT)
			return fail(out, cap, status, 404, "unknown space");
		if (!parse_uint(seg[4], seg_len[4], 65535, &address))
			return fail(out, cap, status, 400, "address must be a number from 0 to 65535");
		if (seg_len[5] == 5 && memcmp(seg[5], "write", 5) == 0)
			event = ACTION_ON_WRITE;
		else if (seg_len[5] != 4 || memcmp(seg[5], "read", 4) != 0)
			return fail(out, cap, status, 404, "event must be read or write");
		error = parse_body(body, &parsed);
		if (error)
			return fail(out, cap, status, 400, error);
		if (!parsed.has[F_NAME] || !parsed.has[F_VALUE] || parsed.has[F_VALUES] ||
		    parsed.has[F_PERCENT] || parsed.has[F_DELAY_MS] || parsed.has[F_EXCEPTION_CODE] ||
		    parsed.has[F_MODE])
			return fail(out, cap, status, 400, "body must be {\"name\": STRING, \"value\": NUMBER}");
		if (!device_action_param_get(dev, (space_kind_t)space, address, event, parsed.name, &current))
			return fail(out, cap, status, 404, "no such action parameter");
		next.type = current.type;
		if (current.type == VAL_BOOL)
		{
			if (parsed.num[F_VALUE] != 0 &&
			    parsed.num[F_VALUE] != 1)
				return fail(out, cap, status, 422, "a boolean parameter takes 0 or 1");
			next.b = parsed.num[F_VALUE] != 0;
		}
		else
			next.n = parsed.num[F_VALUE];
		device_action_param_set(dev, (space_kind_t)space, address, event, parsed.name, &next);
		LOG_INFO("control: device %u action %s %u %s param set", dev->unit_id,
			 SPACE_NAMES[space], address, event == ACTION_ON_WRITE ? "write" : "read");
		return done(out, cap, status);
	}

	if (seg_len[2] == 6 && memcmp(seg[2], "faults", 6) == 0)
	{
		fault_kind_t kind = FAULT_KIND_COUNT;

		if (nseg == 3)
		{
			if (!get)
				return fail(out, cap, status, 405, "method not allowed");
			used = list_faults(dev, out, cap);
			return used < cap ? used : fail(out, cap, status, 500, "response too large");
		}
		if (nseg != 4)
			return fail(out, cap, status, 404, "unknown path");
		for (u32 k = 0; k < FAULT_KIND_COUNT; k++)
		{
			if (strlen(fault_kind_name((fault_kind_t)k)) == seg_len[3] &&
			    memcmp(fault_kind_name((fault_kind_t)k), seg[3], seg_len[3]) == 0)
				kind = (fault_kind_t)k;
		}
		if (kind == FAULT_KIND_COUNT)
			return fail(out, cap, status, 404, "unknown fault kind");
		if (del)
		{
			if ((dev->faults.present & FAULT_BIT(kind)) == 0)
				return fail(out, cap, status, 404, "fault is not set");
			dev->faults.present &= (u8)~FAULT_BIT(kind);
			memset(&dev->faults.by_kind[kind], 0, sizeof(dev->faults.by_kind[kind]));
			dev->fault_runtime.by_kind[kind] = (fault_schedule_state_t){ 0 };
			LOG_INFO("control: device %u fault %s cleared", dev->unit_id, fault_kind_name(kind));
			return done(out, cap, status);
		}
		if (!put_m)
			return fail(out, cap, status, 405, "method not allowed");
		error = parse_body(body, &parsed);
		if (error)
			return fail(out, cap, status, 400, error);
		return put_fault(obs, dev, kind, &parsed, out, cap, status);
	}

	for (u32 s = 0; s < SPACE_COUNT; s++)
	{
		if (strlen(SPACE_NAMES[s]) == seg_len[2] && memcmp(SPACE_NAMES[s], seg[2], seg_len[2]) == 0)
			space = s;
	}
	if (space == SPACE_COUNT)
		return fail(out, cap, status, 404, "unknown space");
	if (nseg < 4 || nseg > 5 ||
	    (nseg == 5 && (seg_len[4] != 5 || memcmp(seg[4], "value", 5) != 0)))
		return fail(out, cap, status, 404, "unknown path");
	if (!parse_uint(seg[3], seg_len[3], 65535, &address))
		return fail(out, cap, status, 400, "address must be a number from 0 to 65535");
	if (!get && !put_m)
		return fail(out, cap, status, 405, "method not allowed");

	device_tick(dev, now_ms);
	if (nseg == 5)
	{
		val_t val = { 0 };

		if (get)
		{
			if (!device_get_point(dev, (space_kind_t)space, address, &val))
				return fail(out, cap, status, 422, value_refusal(dev, (space_kind_t)space, address));
			if (val.type != VAL_BOOL && !isfinite(val.n))
				return (u32)snprintf(out, cap, "{\"value\":null}");
			if (val.type == VAL_BOOL)
				return (u32)snprintf(out, cap, "{\"value\":%d}", val.b ? 1 : 0);
			return (u32)snprintf(out, cap, "{\"value\":%.15g}", val.n);
		}
		error = parse_body(body, &parsed);
		if (error)
			return fail(out, cap, status, 400, error);
		if (!parsed.has[F_VALUE] || parsed.has[F_VALUES] || parsed.has[F_PERCENT] ||
		    parsed.has[F_DELAY_MS] || parsed.has[F_EXCEPTION_CODE] || parsed.has[F_MODE] ||
		    parsed.has[F_NAME])
			return fail(out, cap, status, 400, "body must be {\"value\": NUMBER}");
		if (space_is_bit((space_kind_t)space))
		{
			val.type = VAL_BOOL;
			val.b    = parsed.num[F_VALUE] != 0;
		}
		else
		{
			val.type = VAL_NUM;
			val.n    = parsed.num[F_VALUE];
		}
		if (!device_set_point(dev, (space_kind_t)space, address, &val))
			return fail(out, cap, status, 422, value_refusal(dev, (space_kind_t)space, address));
		return done(out, cap, status);
	}

	if (get)
	{
		u16	regs[MAX_BITS];
		u32	count = 1;
		bool	bits = space_is_bit((space_kind_t)space);
		u32	limit = bits ? MAX_BITS : MAX_REGISTERS;
		const char* c = strstr(query, "count=");

		if (c && (c == query || c[-1] == '&'))
		{
			u32 len = (u32)strcspn(c + 6, "&");

			if (!parse_uint(c + 6, len, limit, &count) || count == 0)
				return fail(out, cap, status, 400, bits ? "count must be 1-2000" : "count must be 1-125");
		}
		if (!device_read(dev, (space_kind_t)space, address, count, regs))
			return fail(out, cap, status, 422, "address range is not defined");
		put(out, cap, &used, "{\"values\":[");
		for (u32 i = 0; i < count; i++)
			put(out, cap, &used, "%s%u", i ? "," : "", regs[i]);
		put(out, cap, &used, "]}");
		return used < cap ? used : fail(out, cap, status, 500, "response too large");
	}

	{
		u16	regs[MAX_VALUES];
		bool	bits = space_is_bit((space_kind_t)space);

		error = parse_body(body, &parsed);
		if (error)
			return fail(out, cap, status, 400, error);
		if (!parsed.has[F_VALUES] || parsed.value_count == 0 ||
		    parsed.has[F_VALUE] || parsed.has[F_PERCENT] || parsed.has[F_DELAY_MS] ||
		    parsed.has[F_EXCEPTION_CODE] || parsed.has[F_MODE] || parsed.has[F_NAME])
			return fail(out, cap, status, 400, "body must be {\"values\": [NUMBER, ...]}");
		if (parsed.value_count > (bits ? MAX_BITS : MAX_REGISTERS))
			return fail(out, cap, status, 400, "too many values");
		for (u32 i = 0; i < parsed.value_count; i++)
		{
			if (!whole(parsed.values[i], bits ? 1 : 65535))
				return fail(out, cap, status, 400,
					    bits ? "values must be 0 or 1" : "values must be integers from 0 to 65535");
			regs[i] = (u16)parsed.values[i];
		}
		if (!device_write(dev, (space_kind_t)space, address, parsed.value_count, regs))
			return fail(out, cap, status, 422,
				    "write refused: undefined address or a point driven by a machine or the host");
		return done(out, cap, status);
	}
}
