/**
 * @file
 * @brief Frame decoder: ADU (MBAP, RTU, ASCII) and PDU fields of every function code mogbus
 *        supports, as text or JSON.
 *
 * One walk produces both formats through a small writer. The direction of a PDU (request or
 * response) is found by decoding it both ways with the output muted and comparing the problems
 * each reading finds.
 */
#include "decode/decode.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "modbus/mei_canopen.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct dec_t dec_t;
typedef struct cur_t cur_t;
typedef enum dir_t dir_t;

#define DEC_DEPTH	12
#define DEC_MSGS	8
#define DEC_MAX_ITEMS	64

/** @brief The direction a PDU was decoded as. */
enum dir_t
{
	DIR_REQUEST,
	DIR_RESPONSE,
	DIR_UNKNOWN,	/**< a function code mogbus does not define: the payload stays raw */
};

/** @brief The output writer, and the problems found while decoding one frame. */
struct dec_t
{
	char*	out;
	u32	cap;
	u32	len;
	bool	json;
	bool	mute;		/**< a trial decode: nothing is written, problems are only counted */
	bool	trunc;
	/** @brief The open containers: whether each is an array, and its member count. */
	struct
	{
		bool	arr;
		u32	n;
	}	stack[DEC_DEPTH];
	u32	depth;
	/** @brief The problems of the current frame, listed after its fields. */
	struct
	{
		bool	fatal;
		char	text[128];
	}	msgs[DEC_MSGS];
	u32	nmsg;
	u32	nfatal;		/**< structural problems of the current frame */
	u32	nwarn;		/**< semantic problems (values a server would refuse) */
	u32	dropped;
	bool	any_fatal;
	u32	base;		/**< frame offset of PDU byte 0 */
};

/** @brief A read position inside a byte range; `off` is the frame offset of p[0]. */
struct cur_t
{
	const u8*	p;
	u32		n;
	u32		pos;
	u32		off;
	bool		bad;
};

/* ---- writer ---- */

static void __attribute__((format(printf, 2, 3)))
put(dec_t* d, const char* fmt, ...)
{
	va_list	ap;
	int	n;
	u32	room;

	if (d->mute ||
	    d->trunc)
		return;
	room = d->cap - d->len;
	va_start(ap, fmt);
	n = vsnprintf(d->out + d->len, room, fmt, ap);
	va_end(ap);
	if (n < 0)
	{
		d->out[d->len] = '\0';
		d->trunc       = true;
	}
	else if ((u32)n >= room)
	{
		d->len   = d->cap - 1;
		d->trunc = true;
	}
	else
		d->len += (u32)n;
}

static void
indent(dec_t* d)
{
	for (u32 i = 0; i < d->depth; i++)
		put(d, "  ");
}

static void
jstr(dec_t* d, const char* s)
{
	put(d, "\"");
	for (; *s; s++)
	{
		u8 ch = (u8)*s;

		if (ch == '"' ||
		    ch == '\\')
			put(d, "\\%c", ch);
		else if (ch < 0x20 ||
			 ch > 0x7E)
			put(d, "\\u%04X", ch);
		else
			put(d, "%c", ch);
	}
	put(d, "\"");
}

/** @brief Starts a member: separator, indentation and the key (or the array index in text). */
static void
item(dec_t* d, const char* key)
{
	if (d->mute)
		return;
	if (d->json)
	{
		if (d->stack[d->depth].n++)
			put(d, ",");
		if (!d->stack[d->depth].arr)
			put(d, "\"%s\":", key);
		return;
	}
	indent(d);
	if (d->stack[d->depth].arr)
		put(d, "[%u]:", d->stack[d->depth].n++);
	else
		put(d, "%s:", key);
}

static void
w_open(dec_t* d, const char* key, bool arr)
{
	if (d->mute)
		return;
	item(d, key);
	put(d, d->json ? (arr ? "[" : "{") : "\n");
	if (d->depth + 1 < DEC_DEPTH)
		d->depth++;
	d->stack[d->depth].arr = arr;
	d->stack[d->depth].n   = 0;
}

static void
w_close(dec_t* d)
{
	if (d->mute)
		return;
	if (d->depth > 0)
		d->depth--;
	if (d->json)
		put(d, d->stack[d->depth + 1].arr ? "]" : "}");
}

static void
w_str(dec_t* d, const char* key, const char* s)
{
	if (d->mute)
		return;
	item(d, key);
	if (d->json)
		jstr(d, s);
	else
	{
		put(d, " ");
		for (; *s; s++)
			put(d, "%c", (u8)*s >= 0x20 && (u8)*s <= 0x7E ? *s : '.');
		put(d, "\n");
	}
}

static void
w_bool(dec_t* d, const char* key, bool v)
{
	if (d->mute)
		return;
	item(d, key);
	put(d, d->json ? "%s" : " %s\n", v ? "true" : "false");
}

/** @brief A number; `hex` digits of its hexadecimal form in text (0 = none); `name` follows it. */
static void
w_num(dec_t* d, const char* key, u64 v, u32 hex, const char* name)
{
	if (d->mute)
		return;
	item(d, key);
	if (d->json)
	{
		put(d, "%llu", (unsigned long long)v);
		if (name)
		{
			put(d, ",\"%s_name\":", key);
			jstr(d, name);
		}
		return;
	}
	put(d, " %llu", (unsigned long long)v);
	if (hex)
		put(d, " (0x%0*llX)", (int)hex, (unsigned long long)v);
	if (name)
		put(d, " %s", name);
	put(d, "\n");
}

static void
w_hex(dec_t* d, const char* key, const u8* p, u32 n)
{
	if (d->mute)
		return;
	item(d, key);
	put(d, d->json ? "\"" : " ");
	for (u32 i = 0; i < n; i++)
		put(d, d->json || i == 0 ? "%02X" : " %02X", p[i]);
	put(d, d->json ? "\"" : "\n");
}

/** @brief 16-bit big-endian values: decimal list (and their hex in text). */
static void
w_regs(dec_t* d, const char* key, const u8* p, u32 n)
{
	u32 count = n / 2;

	if (d->mute)
		return;
	item(d, key);
	put(d, d->json ? "[" : " ");
	for (u32 i = 0; i < count; i++)
	{
		if (i)
			put(d, d->json ? "," : " ");
		put(d, "%u", get_be16(p + 2 * i));
	}
	if (d->json)
		put(d, "]");
	else
	{
		put(d, count ? "  [hex" : "(none)");
		for (u32 i = 0; i < count; i++)
			put(d, " %04X", get_be16(p + 2 * i));
		put(d, count ? "]\n" : "\n");
	}
}

/** @brief The first `nbits` bits of `p`, bit 0 of byte 0 first (coil order). */
static void
w_bits(dec_t* d, const char* key, const u8* p, u32 nbits)
{
	if (d->mute)
		return;
	item(d, key);
	put(d, d->json ? "[" : " ");
	for (u32 i = 0; i < nbits; i++)
	{
		if (i)
			put(d, d->json ? "," : " ");
		put(d, "%u", (p[i / 8] >> (i % 8)) & 1);
	}
	put(d, d->json ? "]" : (nbits ? "\n" : "(none)\n"));
}

static void
w_listitem(dec_t* d, const char* s)
{
	if (d->mute)
		return;
	if (d->json)
	{
		if (d->stack[d->depth].n++)
			put(d, ",");
		jstr(d, s);
		return;
	}
	indent(d);
	put(d, "- %s\n", s);
}

/* ---- problems ---- */

static void __attribute__((format(printf, 3, 4)))
msg(dec_t* d, bool fatal, const char* fmt, ...)
{
	va_list	ap;

	if (fatal)
		d->nfatal++;
	else
		d->nwarn++;
	if (d->mute)
		return;
	if (d->nmsg >= DEC_MSGS)
	{
		d->dropped++;
		return;
	}
	va_start(ap, fmt);
	vsnprintf(d->msgs[d->nmsg].text, sizeof(d->msgs[d->nmsg].text), fmt, ap);
	va_end(ap);
	d->msgs[d->nmsg++].fatal = fatal;
}

/** @brief Writes `warnings`, `errors` and `ok` of the frame decoded so far, then starts over. */
static void
flush_msgs(dec_t* d)
{
	for (int fatal = 0; fatal < 2; fatal++)
	{
		bool any = false;

		for (u32 i = 0; i < d->nmsg; i++)
			any = any || d->msgs[i].fatal == (fatal != 0);
		if (!any)
			continue;
		w_open(d, fatal ? "errors" : "warnings", true);
		for (u32 i = 0; i < d->nmsg; i++)
		{
			if (d->msgs[i].fatal == (fatal != 0))
				w_listitem(d, d->msgs[i].text);
		}
		w_close(d);
	}
	if (d->dropped)
		w_num(d, "more_problems", d->dropped, 0, NULL);
	w_bool(d, "ok", d->nfatal == 0);
	d->any_fatal = d->any_fatal || d->nfatal != 0;
	d->nmsg      = 0;
	d->nfatal    = 0;
	d->nwarn     = 0;
	d->dropped   = 0;
}

/* ---- cursor ---- */

static u32
left(const cur_t* c)
{
	return c->n - c->pos;
}

static bool
need(dec_t* d, cur_t* c, u32 n, const char* what)
{
	if (c->bad)
		return false;
	if (left(c) >= n)
		return true;
	c->bad = true;
	msg(d, true, "truncated at byte %u: %s needs %u byte%s, %u left", c->off + c->pos, what, n,
	    n == 1 ? "" : "s", left(c));
	return false;
}

/** @brief Reads a big-endian number of `size` bytes (1, 2 or 4) and writes it; 0 when short. */
static u32
rd(dec_t* d, cur_t* c, const char* key, u32 size, bool hex, const char* name)
{
	u32 v = 0;

	if (!need(d, c, size, key))
		return 0;
	for (u32 i = 0; i < size; i++)
		v = (v << 8) | c->p[c->pos++];
	w_num(d, key, v, hex ? size * 2 : 0, name);
	return v;
}

/** @brief Takes up to `want` bytes; a shortfall is a problem and ends the cursor. */
static const u8*
grab(dec_t* d, cur_t* c, u32 want, u32* got, const char* what)
{
	const u8* p = c->p + c->pos;

	if (c->bad)
		want = 0;
	else if (want > left(c))
	{
		msg(d, true, "truncated at byte %u: %s announces %u bytes, %u left", c->off + c->pos, what,
		    want, left(c));
		want   = left(c);
		c->bad = true;
	}
	c->pos += want;
	*got    = want;
	return p;
}

/** @brief A cursor over the next `want` bytes of `c`. */
static cur_t
sub(dec_t* d, cur_t* c, u32 want, const char* what)
{
	cur_t		s;
	u32		off = c->off + c->pos;
	u32		got = 0;
	const u8*	p   = grab(d, c, want, &got, what);

	s = (cur_t){ .p = p, .n = got, .pos = 0, .off = off, .bad = false };
	return s;
}

/** @brief Reports bytes left over after the fields of `what`. */
static void
finish(dec_t* d, cur_t* c, const char* what)
{
	u32 n = left(c);

	if (c->bad ||
	    n == 0)
		return;
	msg(d, true, "%u unexpected trailing byte%s at byte %u after %s", n, n == 1 ? "" : "s",
	    c->off + c->pos, what);
	w_hex(d, "extra", c->p + c->pos, n);
	c->pos = c->n;
}

/* ---- names ---- */

static const char*
fc_name(u8 fc)
{
	switch (fc)
	{
		case FC_READ_COILS                    : return "Read Coils";
		case FC_READ_DISCRETE_INPUTS          : return "Read Discrete Inputs";
		case FC_READ_HOLDING_REGISTERS        : return "Read Holding Registers";
		case FC_READ_INPUT_REGISTERS          : return "Read Input Registers";
		case FC_WRITE_SINGLE_COIL             : return "Write Single Coil";
		case FC_WRITE_SINGLE_REGISTER         : return "Write Single Register";
		case FC_READ_EXCEPTION_STATUS         : return "Read Exception Status";
		case FC_DIAGNOSTICS                   : return "Diagnostics";
		case FC_GET_COM_EVENT_COUNTER         : return "Get Comm Event Counter";
		case FC_GET_COM_EVENT_LOG             : return "Get Comm Event Log";
		case FC_WRITE_MULTIPLE_COILS          : return "Write Multiple Coils";
		case FC_WRITE_MULTIPLE_REGISTERS      : return "Write Multiple Registers";
		case FC_REPORT_SERVER_ID              : return "Report Server ID";
		case FC_READ_FILE_RECORD              : return "Read File Record";
		case FC_WRITE_FILE_RECORD             : return "Write File Record";
		case FC_MASK_WRITE_REGISTER           : return "Mask Write Register";
		case FC_READ_WRITE_MULTIPLE_REGISTERS : return "Read/Write Multiple Registers";
		case FC_READ_FIFO_QUEUE               : return "Read FIFO Queue";
		case FC_ENCAPSULATED_INTERFACE_TRANSPORT: return "Encapsulated Interface Transport";
		default:
			if ((fc >= 65 && fc <= 72) ||
			    (fc >= 100 && fc <= 110))
				return "user-defined";
			return "unknown";
	}
}

static const char*
exception_name(u8 code)
{
	switch (code)
	{
		case EXC_ILLEGAL_FUNCTION         : return "Illegal Function";
		case EXC_ILLEGAL_DATA_ADDRESS     : return "Illegal Data Address";
		case EXC_ILLEGAL_DATA_VALUE       : return "Illegal Data Value";
		case EXC_SLAVE_DEVICE_FAILURE     : return "Slave Device Failure";
		case EXC_ACKNOWLEDGE              : return "Acknowledge";
		case EXC_SLAVE_DEVICE_BUSY        : return "Slave Device Busy";
		case EXC_NEGATIVE_ACKNOWLEDGE     : return "Negative Acknowledge";
		case EXC_MEMORY_PARITY_ERROR      : return "Memory Parity Error";
		case 0x09                         : return "reserved";
		case EXC_GATEWAY_PATH_UNAVAILABLE : return "Gateway Path Unavailable";
		case EXC_GATEWAY_TARGET_FAILED    : return "Gateway Target Device Failed to Respond";
		default                           : return "unknown";
	}
}

static const char*
diag_name(u16 sub_function)
{
	switch (sub_function)
	{
		case DIAG_SUB_RETURN_QUERY_DATA                : return "Return Query Data";
		case DIAG_SUB_RESTART_COMMUNICATIONS           : return "Restart Communications Option";
		case DIAG_SUB_READ_DIAGNOSTIC_REGISTER         : return "Return Diagnostic Register";
		case DIAG_SUB_CHANGE_ASCII_DELIMITER           : return "Change ASCII Input Delimiter";
		case DIAG_SUB_LISTEN_ONLY_MODE                 : return "Force Listen Only Mode";
		case DIAG_SUB_CLEAR_COUNTERS                   : return "Clear Counters and Diagnostic Register";
		case DIAG_SUB_RETURN_BUS_MESSAGE_COUNT         : return "Return Bus Message Count";
		case DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT      : return "Return Bus Communication Error Count";
		case DIAG_SUB_RETURN_BUS_EXCEPTION_ERROR_COUNT : return "Return Bus Exception Error Count";
		case DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT       : return "Return Server Message Count";
		case DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT   : return "Return Server No Response Count";
		case DIAG_SUB_RETURN_SLAVE_NAK_COUNT           : return "Return Server NAK Count";
		case DIAG_SUB_RETURN_SLAVE_BUSY_COUNT          : return "Return Server Busy Count";
		case DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT    : return "Return Bus Character Overrun Count";
		case DIAG_SUB_CLEAR_OVERRUN_COUNTER_AND_FLAG   : return "Clear Overrun Counter and Flag";
		default                                        : return "unknown";
	}
}

static const char*
mei_name(u8 type)
{
	switch (type)
	{
		case MEI_TYPE_READ_DEVICE_ID : return "Read Device Identification";
		case MEI_TYPE_CANOPEN        : return "CANopen General Reference";
		default                      : return "unknown";
	}
}

static const char*
object_name(u8 id)
{
	static const char* const names[] = {
		"VendorName", "ProductCode", "MajorMinorRevision", "VendorUrl", "ProductName",
		"ModelName", "UserApplicationName",
	};

	if (id < sizeof(names) / sizeof(names[0]))
		return names[id];
	return id >= 0x80 ? "private" : "reserved";
}

static const char*
event_name(u8 event)
{
	if (event == 0x00)
		return "communications restart";
	if (event == 0x04)
		return "entered listen only mode";
	if (event & BIT7)
		return "receive event";
	if (event & BIT6)
		return "send event";
	return "unknown";
}

/* ---- PDU: requests ---- */

static void
req_read(dec_t* d, cur_t* c, u32 max)
{
	u32 quantity;

	rd(d, c, "address", 2, true, NULL);
	quantity = rd(d, c, "quantity", 2, false, NULL);
	if (!c->bad &&
	    (quantity == 0 || quantity > max))
		msg(d, false, "quantity %u is outside 1..%u (a server answers exception 03)", quantity, max);
}

/** @brief FC 14/15 sub-requests; FC 15 carries the record data (and its response repeats them). */
static void
file_records(dec_t* d, cur_t* c, bool write, const char* key)
{
	u32	byte_count = rd(d, c, "byte_count", 1, false, NULL);
	cur_t	s;
	u32	i = 0;

	if (c->bad)
		return;
	s = sub(d, c, byte_count, "the sub-requests");
	w_open(d, key, true);
	while (!s.bad &&
	       left(&s) > 0 &&
	       i < DEC_MAX_ITEMS)
	{
		u32 type;
		u32 length;

		w_open(d, NULL, false);
		type = rd(d, &s, "reference_type", 1, true, NULL);
		rd(d, &s, "file_number", 2, false, NULL);
		rd(d, &s, "record_number", 2, false, NULL);
		length = rd(d, &s, "record_length", 2, false, NULL);
		if (!s.bad &&
		    type != 6)
			msg(d, false, "reference type %u is not 6", type);
		if (write &&
		    !s.bad)
		{
			u32		got;
			const u8*	data = grab(d, &s, length * 2, &got, "the record data");

			w_regs(d, "data", data, got);
		}
		w_close(d);
		i++;
	}
	w_close(d);
	finish(d, &s, "the sub-requests");
	if (!c->bad &&
	    (byte_count < (write ? 9u : 7u) || byte_count > 245))
		msg(d, false, "byte count %u is outside %u..245", byte_count, write ? 9u : 7u);
}

/** @brief The CANopen fields after `2B 0D`; requests and responses differ in the options. */
static void
canopen(dec_t* d, cur_t* c, bool response)
{
	u32	control = rd(d, c, "control", 1, true, NULL);
	bool	write   = (control & CANOPEN_CTRL_ACCESS) != 0;
	u32	reserved;
	u32	count;

	if (c->bad)
		return;
	w_str(d, "access", write ? "write (download)" : "read (upload)");
	if (!response &&
	    (control & CANOPEN_CTRL_EXTENDED_CTRL))
		rd(d, c, "extended_control", 1, true, NULL);
	reserved = rd(d, c, "reserved", 1, true, NULL);
	if (!c->bad &&
	    reserved != 0)
		msg(d, false, "the reserved byte is %u, not 0", reserved);
	if (!response &&
	    (control & CANOPEN_CTRL_COUNTER))
		rd(d, c, "counter", 1, true, NULL);
	if (control & CANOPEN_CTRL_NETWORK_ID)
		rd(d, c, "network_id", 1, false, NULL);
	if (!response &&
	    (control & CANOPEN_CTRL_ENCODED_DATA))
		rd(d, c, "data_type", 1, true, NULL);
	rd(d, c, "node", 1, false, NULL);
	rd(d, c, "index", 2, true, NULL);
	rd(d, c, "subindex", 1, false, NULL);
	rd(d, c, "start", 2, false, NULL);
	count = rd(d, c, "count", 2, false, NULL);
	if (c->bad)
		return;
	if (left(c) > 0)
	{
		w_hex(d, "data", c->p + c->pos, left(c));
		if (!response &&
		    !write)
			msg(d, true, "%u unexpected trailing byte%s after the read request", left(c),
			    left(c) == 1 ? "" : "s");
		else if (response &&
			 write)
			msg(d, true, "a write response carries no data");
		else if (response &&
			 left(c) != count)
			msg(d, false, "count is %u but %u data bytes follow", count, left(c));
		else if (!response &&
			 !(control & CANOPEN_CTRL_ENCODED_DATA) &&
			 left(c) != count)
			msg(d, false, "count is %u but %u data bytes follow", count, left(c));
		c->pos = c->n;
	}
	else if (response &&
		 !write &&
		 count != 0)
		msg(d, false, "count is %u but no data follows", count);
	else if (!response &&
		 write &&
		 count != 0)
		msg(d, false, "count is %u but no data follows", count);
}

static void
read_device_id(dec_t* d, cur_t* c, bool response)
{
	static const char* const codes[] = { "basic stream", "regular stream", "extended stream",
					     "individual object" };
	u32 code = c->p[c->pos];

	if (!need(d, c, 1, "read_device_id_code"))
		return;
	rd(d, c, "read_device_id_code", 1, true, code >= 1 && code <= 4 ? codes[code - 1] : "unknown");
	if (!response)
	{
		u32 id = rd(d, c, "object_id", 1, true, NULL);

		if (!c->bad)
			w_str(d, "object_name", object_name((u8)id));
		return;
	}
	{
		u32	level = c->pos < c->n ? c->p[c->pos] : 0;
		u32	more;
		u32	count;

		rd(d, c, "conformity_level", 1, true,
		   level == 0x01 ? "basic, stream" :
		   level == 0x02 ? "regular, stream" :
		   level == 0x03 ? "extended, stream" :
		   level == 0x81 ? "basic, stream and individual" :
		   level == 0x82 ? "regular, stream and individual" :
		   level == 0x83 ? "extended, stream and individual" : "unknown");
		more = rd(d, c, "more_follows", 1, true, NULL);
		rd(d, c, "next_object_id", 1, true, NULL);
		count = rd(d, c, "number_of_objects", 1, false, NULL);
		if (c->bad)
			return;
		if (more != 0x00 &&
		    more != 0xFF)
			msg(d, false, "more_follows is 0x%02X, not 00 or FF", more);
		w_open(d, "objects", true);
		for (u32 i = 0; i < count && !c->bad; i++)
		{
			u32		length;
			u32		got;
			const u8*	value;
			char		text[256];

			w_open(d, NULL, false);
			rd(d, c, "id", 1, true, object_name((u8)(c->pos < c->n ? c->p[c->pos] : 0)));
			length = rd(d, c, "length", 1, false, NULL);
			if (!c->bad)
			{
				value = grab(d, c, length, &got, "the object value");
				for (u32 k = 0; k < got; k++)
					text[k] = (char)value[k];
				text[got] = '\0';
				w_str(d, "value", text);
			}
			w_close(d);
		}
		w_close(d);
	}
}

static void
decode_request(dec_t* d, cur_t* c, u8 fc)
{
	switch (fc)
	{
		case FC_READ_COILS:
		case FC_READ_DISCRETE_INPUTS:
			req_read(d, c, MB_MAX_READ_BITS);
			break;
		case FC_READ_HOLDING_REGISTERS:
		case FC_READ_INPUT_REGISTERS:
			req_read(d, c, MB_MAX_READ_REGISTERS);
			break;
		case FC_WRITE_SINGLE_COIL:
		{
			u32 value;

			rd(d, c, "address", 2, true, NULL);
			value = rd(d, c, "value", 2, true,
				   c->pos + 2 <= c->n ? (get_be16(c->p + c->pos) == 0xFF00 ? "ON" :
					get_be16(c->p + c->pos) == 0 ? "OFF" : "invalid") : NULL);
			if (!c->bad &&
			    value != 0xFF00 &&
			    value != 0)
				msg(d, false, "coil value 0x%04X is neither FF00 nor 0000", value);
			break;
		}
		case FC_WRITE_SINGLE_REGISTER:
			rd(d, c, "address", 2, true, NULL);
			rd(d, c, "value", 2, true, NULL);
			break;
		case FC_READ_EXCEPTION_STATUS:
		case FC_GET_COM_EVENT_COUNTER:
		case FC_GET_COM_EVENT_LOG:
		case FC_REPORT_SERVER_ID:
			break;
		case FC_DIAGNOSTICS:
		{
			u32 sf = rd(d, c, "sub_function", 2, true, NULL);

			if (c->bad)
				break;
			w_str(d, "sub_function_text", diag_name((u16)sf));
			rd(d, c, "data", 2, true,
			   sf == DIAG_SUB_RESTART_COMMUNICATIONS && c->pos + 2 <= c->n &&
			   get_be16(c->p + c->pos) == 0xFF00 ? "clear the event log" : NULL);
			break;
		}
		case FC_WRITE_MULTIPLE_COILS:
		{
			u32		quantity;
			u32		byte_count;
			u32		got;
			const u8*	data;

			rd(d, c, "address", 2, true, NULL);
			quantity   = rd(d, c, "quantity", 2, false, NULL);
			byte_count = rd(d, c, "byte_count", 1, false, NULL);
			if (c->bad)
				break;
			data = grab(d, c, byte_count, &got, "the coil data");
			w_hex(d, "data", data, got);
			w_bits(d, "coils", data, quantity < got * 8 ? quantity : got * 8);
			if (quantity == 0 ||
			    quantity > MB_MAX_WRITE_BITS)
				msg(d, false, "quantity %u is outside 1..%u", quantity, MB_MAX_WRITE_BITS);
			else if (byte_count != (quantity + 7) / 8)
				msg(d, false, "byte count %u does not match quantity %u (%u expected)", byte_count,
				    quantity, (quantity + 7) / 8);
			break;
		}
		case FC_WRITE_MULTIPLE_REGISTERS:
		{
			u32		quantity;
			u32		byte_count;
			u32		got;
			const u8*	data;

			rd(d, c, "address", 2, true, NULL);
			quantity   = rd(d, c, "quantity", 2, false, NULL);
			byte_count = rd(d, c, "byte_count", 1, false, NULL);
			if (c->bad)
				break;
			data = grab(d, c, byte_count, &got, "the register data");
			w_regs(d, "values", data, got);
			if (quantity == 0 ||
			    quantity > MB_MAX_WRITE_REGISTERS)
				msg(d, false, "quantity %u is outside 1..%u", quantity, MB_MAX_WRITE_REGISTERS);
			else if (byte_count != quantity * 2)
				msg(d, false, "byte count %u does not match quantity %u (%u expected)", byte_count,
				    quantity, quantity * 2);
			break;
		}
		case FC_READ_FILE_RECORD:
			file_records(d, c, false, "sub_requests");
			break;
		case FC_WRITE_FILE_RECORD:
			file_records(d, c, true, "sub_requests");
			break;
		case FC_MASK_WRITE_REGISTER:
			rd(d, c, "address", 2, true, NULL);
			rd(d, c, "and_mask", 2, true, NULL);
			rd(d, c, "or_mask", 2, true, NULL);
			break;
		case FC_READ_WRITE_MULTIPLE_REGISTERS:
		{
			u32		read_quantity;
			u32		write_quantity;
			u32		byte_count;
			u32		got;
			const u8*	data;

			rd(d, c, "read_address", 2, true, NULL);
			read_quantity = rd(d, c, "read_quantity", 2, false, NULL);
			rd(d, c, "write_address", 2, true, NULL);
			write_quantity = rd(d, c, "write_quantity", 2, false, NULL);
			byte_count     = rd(d, c, "write_byte_count", 1, false, NULL);
			if (c->bad)
				break;
			data = grab(d, c, byte_count, &got, "the register data");
			w_regs(d, "values", data, got);
			if (read_quantity == 0 ||
			    read_quantity > MB_MAX_READ_REGISTERS ||
			    write_quantity == 0 ||
			    write_quantity > 121 ||
			    byte_count != write_quantity * 2)
				msg(d, false, "quantities %u/%u or byte count %u are out of range", read_quantity,
				    write_quantity, byte_count);
			break;
		}
		case FC_READ_FIFO_QUEUE:
			rd(d, c, "fifo_address", 2, true, NULL);
			break;
		case FC_ENCAPSULATED_INTERFACE_TRANSPORT:
		{
			u32 mei;

			if (!need(d, c, 1, "mei_type"))
				break;
			mei = rd(d, c, "mei_type", 1, true, mei_name(c->p[c->pos]));
			if (mei == MEI_TYPE_READ_DEVICE_ID)
				read_device_id(d, c, false);
			else if (mei == MEI_TYPE_CANOPEN)
				canopen(d, c, false);
			else if (left(c) > 0)
			{
				w_hex(d, "data", c->p + c->pos, left(c));
				c->pos = c->n;
				msg(d, false, "MEI type 0x%02X is not supported", mei);
			}
			break;
		}
		default:
			if (left(c) > 0)
			{
				w_hex(d, "data", c->p + c->pos, left(c));
				c->pos = c->n;
			}
			break;
	}
	finish(d, c, "the request");
}

/* ---- PDU: responses ---- */

static void
resp_bit_data(dec_t* d, cur_t* c)
{
	u32		byte_count = rd(d, c, "byte_count", 1, false, NULL);
	u32		got;
	const u8*	data;

	if (c->bad)
		return;
	data = grab(d, c, byte_count, &got, "the coil data");
	w_hex(d, "data", data, got);
	w_bits(d, "bits", data, got * 8);
	if (byte_count == 0)
		msg(d, false, "byte count is 0");
}

static void
resp_register_data(dec_t* d, cur_t* c, const char* key)
{
	u32		byte_count = rd(d, c, "byte_count", 1, false, NULL);
	u32		got;
	const u8*	data;

	if (c->bad)
		return;
	data = grab(d, c, byte_count, &got, "the register data");
	w_regs(d, key, data, got);
	if (byte_count == 0 ||
	    byte_count % 2 != 0)
		msg(d, false, "byte count %u is not a positive even number", byte_count);
}

static void
decode_response(dec_t* d, cur_t* c, u8 fc)
{
	switch (fc)
	{
		case FC_READ_COILS:
		case FC_READ_DISCRETE_INPUTS:
			resp_bit_data(d, c);
			break;
		case FC_READ_HOLDING_REGISTERS:
		case FC_READ_INPUT_REGISTERS:
			resp_register_data(d, c, "values");
			break;
		case FC_WRITE_SINGLE_COIL:
		case FC_WRITE_SINGLE_REGISTER:
			rd(d, c, "address", 2, true, NULL);
			rd(d, c, "value", 2, true, NULL);
			break;
		case FC_READ_EXCEPTION_STATUS:
			rd(d, c, "output_data", 1, true, NULL);
			break;
		case FC_DIAGNOSTICS:
		{
			u32 sf = rd(d, c, "sub_function", 2, true, NULL);

			if (c->bad)
				break;
			w_str(d, "sub_function_text", diag_name((u16)sf));
			rd(d, c, "data", 2, true, NULL);
			break;
		}
		case FC_GET_COM_EVENT_COUNTER:
			rd(d, c, "status", 2, true, c->pos + 2 <= c->n ?
			   (get_be16(c->p + c->pos) == 0xFFFF ? "busy" :
			    get_be16(c->p + c->pos) == 0 ? "ready" : "unknown") : NULL);
			rd(d, c, "event_count", 2, false, NULL);
			break;
		case FC_GET_COM_EVENT_LOG:
		{
			u32	byte_count = rd(d, c, "byte_count", 1, false, NULL);
			cur_t	s;

			if (c->bad)
				break;
			s = sub(d, c, byte_count, "the event log");
			rd(d, &s, "status", 2, true, s.pos + 2 <= s.n ?
			   (get_be16(s.p + s.pos) == 0xFFFF ? "busy" :
			    get_be16(s.p + s.pos) == 0 ? "ready" : "unknown") : NULL);
			rd(d, &s, "event_count", 2, false, NULL);
			rd(d, &s, "message_count", 2, false, NULL);
			if (!s.bad)
			{
				u32 i = 0;

				w_open(d, "events", true);
				while (left(&s) > 0 &&
				       i < DEC_MAX_ITEMS)
				{
					w_open(d, NULL, false);
					rd(d, &s, "event", 1, true, event_name(s.p[s.pos]));
					w_close(d);
					i++;
				}
				w_close(d);
				if (byte_count > 64 + 6)
					msg(d, false, "more than 64 events");
			}
			finish(d, &s, "the event log");
			break;
		}
		case FC_WRITE_MULTIPLE_COILS:
		case FC_WRITE_MULTIPLE_REGISTERS:
			rd(d, c, "address", 2, true, NULL);
			rd(d, c, "quantity", 2, false, NULL);
			break;
		case FC_REPORT_SERVER_ID:
		{
			u32	byte_count = rd(d, c, "byte_count", 1, false, NULL);
			cur_t	s;

			if (c->bad)
				break;
			s = sub(d, c, byte_count, "the server id data");
			rd(d, &s, "server_id", 1, true, NULL);
			rd(d, &s, "run_indicator", 1, true, s.pos < s.n ?
			   (s.p[s.pos] == 0xFF ? "ON" : s.p[s.pos] == 0 ? "OFF" : "unknown") : NULL);
			if (!s.bad &&
			    left(&s) > 0)
			{
				w_hex(d, "additional_data", s.p + s.pos, left(&s));
				s.pos = s.n;
			}
			break;
		}
		case FC_READ_FILE_RECORD:
		{
			u32	data_length = rd(d, c, "data_length", 1, false, NULL);
			cur_t	s;
			u32	i = 0;

			if (c->bad)
				break;
			s = sub(d, c, data_length, "the sub-responses");
			w_open(d, "sub_responses", true);
			while (!s.bad &&
			       left(&s) > 0 &&
			       i < DEC_MAX_ITEMS)
			{
				u32 length;

				w_open(d, NULL, false);
				length = rd(d, &s, "file_response_length", 1, false, NULL);
				if (length == 0)
					msg(d, true, "file response length is 0");
				else if (!s.bad)
				{
					u32		got;
					u32		type = rd(d, &s, "reference_type", 1, true, NULL);
					const u8*	data;

					data = grab(d, &s, length - 1, &got, "the record data");
					w_regs(d, "data", data, got);
					if (type != 6)
						msg(d, false, "reference type %u is not 6", type);
					if (length % 2 == 0)
						msg(d, false, "record data of %u bytes is not whole registers",
						    length - 1);
				}
				w_close(d);
				i++;
			}
			w_close(d);
			finish(d, &s, "the sub-responses");
			break;
		}
		case FC_WRITE_FILE_RECORD:
			file_records(d, c, true, "sub_responses");
			break;
		case FC_MASK_WRITE_REGISTER:
			rd(d, c, "address", 2, true, NULL);
			rd(d, c, "and_mask", 2, true, NULL);
			rd(d, c, "or_mask", 2, true, NULL);
			break;
		case FC_READ_WRITE_MULTIPLE_REGISTERS:
			resp_register_data(d, c, "values");
			break;
		case FC_READ_FIFO_QUEUE:
		{
			u32	byte_count = rd(d, c, "byte_count", 2, false, NULL);
			cur_t	s;
			u32	count;

			if (c->bad)
				break;
			s     = sub(d, c, byte_count, "the FIFO data");
			count = rd(d, &s, "fifo_count", 2, false, NULL);
			if (!s.bad)
			{
				u32		got;
				const u8*	data = grab(d, &s, left(&s), &got, "the FIFO values");

				w_regs(d, "values", data, got);
				if (byte_count != 2 + count * 2 ||
				    count > 31)
					msg(d, false, "FIFO count %u does not match byte count %u (31 values at most)",
					    count, byte_count);
			}
			break;
		}
		case FC_ENCAPSULATED_INTERFACE_TRANSPORT:
		{
			u32 mei;

			if (!need(d, c, 1, "mei_type"))
				break;
			mei = rd(d, c, "mei_type", 1, true, mei_name(c->p[c->pos]));
			if (mei == MEI_TYPE_READ_DEVICE_ID)
				read_device_id(d, c, true);
			else if (mei == MEI_TYPE_CANOPEN)
				canopen(d, c, true);
			else if (left(c) > 0)
			{
				w_hex(d, "data", c->p + c->pos, left(c));
				c->pos = c->n;
				msg(d, false, "MEI type 0x%02X is not supported", mei);
			}
			break;
		}
		default:
			if (left(c) > 0)
			{
				w_hex(d, "data", c->p + c->pos, left(c));
				c->pos = c->n;
			}
			break;
	}
	finish(d, c, "the response");
}

/** @brief `AB FF len 0D AE|CE payload`, the CANopen failure; `c` is just after the function code. */
static void
canopen_failure(dec_t* d, cur_t* c)
{
	u32	length;
	cur_t	s;
	u32	failure;

	rd(d, c, "marker", 1, true, "extended failure");
	length = rd(d, c, "length", 2, false, NULL);
	if (c->bad)
		return;
	s = sub(d, c, length, "the failure");
	rd(d, &s, "mei_type", 1, true, s.pos < s.n ? mei_name(s.p[s.pos]) : NULL);
	failure = rd(d, &s, "failure", 1, true,
		     s.pos < s.n ? (s.p[s.pos] == 0xAE ? "unsupported option" :
				    s.p[s.pos] == 0xCE ? "service failure" : "unknown") : NULL);
	if (s.bad)
		return;
	if (failure == 0xAE)
		rd(d, &s, "supported_control", 1, true, NULL);
	else if (failure == 0xCE &&
		 need(d, &s, 4, "error_code"))
	{
		u32		code = (u32)s.p[s.pos] | ((u32)s.p[s.pos + 1] << 8) | ((u32)s.p[s.pos + 2] << 16) |
				       ((u32)s.p[s.pos + 3] << 24);
		const char*	name;

		s.pos += 4;
		switch (code)
		{
			case CANOPEN_ERR_OBJECT_NOT_FOUND      : name = "object does not exist"; break;
			case CANOPEN_ERR_INVALID_COUNT         : name = "invalid count or length"; break;
			case CANOPEN_ERR_LENGTH_TOO_HIGH       : name = "length exceeds the object"; break;
			case CANOPEN_ERR_ACCESS_REFUSED        : name = "access refused"; break;
			case CANOPEN_ERR_RESPONSE_TOO_LARGE    : name = "response too large"; break;
			case CANOPEN_ERR_SERVICE_NOT_SUPPORTED : name = "service not supported"; break;
			default                                : name = NULL; break;
		}
		w_num(d, "error_code", code, 8, name);
	}
	finish(d, &s, "the failure");
}

/** @brief Decodes `pdu` as `dir` and writes its fields into the open object. */
static void
decode_pdu(dec_t* d, const u8* pdu, u32 len, dir_t dir, const char* reason)
{
	cur_t	c = { .p = pdu, .n = len, .pos = 0, .off = d->base, .bad = false };
	u8	fc;
	bool	exception;
	u8	base;

	if (len == 0)
	{
		msg(d, true, "empty PDU: no function code");
		return;
	}
	fc        = pdu[0];
	exception = dir == DIR_RESPONSE &&
		    (fc & 0x80);
	base      = exception ? (u8)(fc & 0x7F) : fc;
	c.pos     = 1;
	w_num(d, "function", base, 2, fc_name(base));
	w_str(d, "direction", dir == DIR_REQUEST ? "request" : dir == DIR_RESPONSE ? "response" : "unknown");
	if (reason)
		w_str(d, "direction_reason", reason);
	if (exception)
	{
		if (fc == (FC_ENCAPSULATED_INTERFACE_TRANSPORT | 0x80) &&
		    len > 1 &&
		    pdu[1] == 0xFF)
			canopen_failure(d, &c);
		else
		{
			rd(d, &c, "exception", 1, true, c.pos < c.n ? exception_name(c.p[c.pos]) : NULL);
		}
		finish(d, &c, "the exception");
		return;
	}
	if (dir == DIR_RESPONSE)
		decode_response(d, &c, fc);
	else if (dir == DIR_REQUEST)
		decode_request(d, &c, fc);
	else if (left(&c) > 0)
		w_hex(d, "data", c.p + c.pos, left(&c));
}

/** @brief Problem level of reading `pdu` as `dir`: 0 clean, 1 refused values, 2 structural. */
static u32
trial(dec_t* d, const u8* pdu, u32 len, dir_t dir)
{
	u32 fatal = d->nfatal;
	u32 warn  = d->nwarn;
	u32 tier;

	d->mute = true;
	decode_pdu(d, pdu, len, dir, NULL);
	d->mute = false;
	tier    = d->nfatal > fatal ? 2 : (d->nwarn > warn ? 1 : 0);
	d->nfatal = fatal;
	d->nwarn  = warn;
	return tier;
}

static dir_t
pick_direction(dec_t* d, const u8* pdu, u32 len, decode_direction_t want, const char** reason)
{
	u8	fc;
	u32	request;
	u32	response;

	*reason = NULL;
	if (want == DECODE_DIRECTION_REQUEST)
		return DIR_REQUEST;
	if (want == DECODE_DIRECTION_RESPONSE)
		return DIR_RESPONSE;
	if (len == 0)
		return DIR_REQUEST;
	fc = pdu[0];
	if (fc & 0x80)
	{
		*reason = "the exception bit of the function code is set";
		return DIR_RESPONSE;
	}
	if (!modbus_fc_is_builtin(fc))
	{
		*reason = "function code is not defined by mogbus, so the payload stays raw";
		return DIR_UNKNOWN;
	}
	request  = trial(d, pdu, len, DIR_REQUEST);
	response = trial(d, pdu, len, DIR_RESPONSE);
	if (request == 0 &&
	    response == 0)
	{
		*reason = "length and fields fit both a request and a response; assumed request";
		return DIR_REQUEST;
	}
	if (request != response)
	{
		*reason = request < response ?
			  "length and fields fit the request layout, not the response layout" :
			  "length and byte count fit the response layout, not the request layout";
		return request < response ? DIR_REQUEST : DIR_RESPONSE;
	}
	if (len >= 2 &&
	    pdu[1] == len - 2 &&
	    (fc == FC_READ_COILS || fc == FC_READ_DISCRETE_INPUTS || fc == FC_READ_HOLDING_REGISTERS ||
	     fc == FC_READ_INPUT_REGISTERS || fc == FC_READ_WRITE_MULTIPLE_REGISTERS))
	{
		*reason = "neither layout fits cleanly; the byte count matches the length, assumed response";
		return DIR_RESPONSE;
	}
	*reason = "neither layout fits cleanly; assumed request";
	return DIR_REQUEST;
}

static void
emit_pdu(dec_t* d, const u8* pdu, u32 len, decode_direction_t want)
{
	const char*	reason;
	dir_t		dir = pick_direction(d, pdu, len, want, &reason);

	w_open(d, "pdu", false);
	decode_pdu(d, pdu, len, dir, reason);
	w_close(d);
}

/* ---- ADUs ---- */

static void
emit_mbap(dec_t* d, const u8* data, u32 size, decode_direction_t want)
{
	cur_t	c = { .p = data, .n = size, .pos = 0, .off = 0, .bad = false };
	u32	protocol;
	u32	length;
	u32	pdu_len;

	w_open(d, "adu", false);
	rd(d, &c, "transaction", 2, true, NULL);
	protocol = rd(d, &c, "protocol", 2, true, NULL);
	length   = rd(d, &c, "length", 2, false, NULL);
	rd(d, &c, "unit", 1, false, NULL);
	w_close(d);
	if (c.bad)
		return;
	if (protocol != 0)
		msg(d, true, "protocol id is 0x%04X, Modbus uses 0", protocol);
	pdu_len = length > 0 ? length - 1 : 0;
	if (pdu_len != size - 7)
		msg(d, true, "the length field says %u byte%s follow it, the frame has %u", length,
		    length == 1 ? "" : "s", size - 6);
	if (pdu_len > size - 7)
		pdu_len = size - 7;
	d->base = 7;
	emit_pdu(d, data + 7, pdu_len, want);
}

static void
emit_rtu(dec_t* d, const u8* data, u32 size, decode_direction_t want)
{
	u32	got;
	u32	expected;

	w_open(d, "adu", false);
	if (size >= 1)
		w_num(d, "address", data[0], 2, data[0] == 0 ? "broadcast" : NULL);
	if (size < 4)
	{
		msg(d, true, "an RTU frame needs an address, a function code and a CRC (4 bytes), got %u", size);
		if (size > 1)
			w_hex(d, "data", data + 1, size - 1);
		w_close(d);
		return;
	}
	got      = (u32)data[size - 2] | ((u32)data[size - 1] << 8);
	expected = modbus_crc16(data, size - 2);
	w_num(d, "crc", got, 4, NULL);
	w_bool(d, "crc_ok", got == expected);
	if (got != expected)
	{
		w_num(d, "crc_expected", expected, 4, NULL);
		msg(d, true, "CRC mismatch: the frame has 0x%04X, the data gives 0x%04X", got, expected);
	}
	w_close(d);
	d->base = 1;
	emit_pdu(d, data + 1, size - 3, want);
}

static int
hex_digit(u8 ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	return -1;
}

static void
emit_ascii(dec_t* d, const u8* data, u32 size, decode_direction_t want)
{
	u8	bin[256];
	u32	n = 0;
	u32	end;
	u32	i = 1;
	bool	hex_ok = true;

	w_open(d, "adu", false);
	if (size == 0 ||
	    data[0] != ':')
	{
		msg(d, true, "an ASCII frame starts with ':'");
		w_close(d);
		return;
	}
	end = size;
	if (size >= 3 &&
	    data[size - 2] == '\r' &&
	    data[size - 1] == '\n')
		end = size - 2;
	else
	{
		msg(d, true, "the frame does not end with CR LF");
		while (end > 1 &&
		       (data[end - 1] == '\r' || data[end - 1] == '\n'))
			end--;
	}
	while (i + 1 < end &&
	       n < sizeof(bin))
	{
		int hi = hex_digit(data[i]);
		int lo = hex_digit(data[i + 1]);

		if (hi < 0 ||
		    lo < 0)
		{
			msg(d, true, "invalid hex digit at character %u", hi < 0 ? i : i + 1);
			hex_ok = false;
			break;
		}
		bin[n++] = (u8)((hi << 4) | lo);
		i += 2;
	}
	if (hex_ok &&
	    i < end)
		msg(d, true, i + 1 == end ? "odd number of hex digits" : "the frame is longer than 255 bytes");
	if (n >= 1)
		w_num(d, "address", bin[0], 2, bin[0] == 0 ? "broadcast" : NULL);
	if (!hex_ok ||
	    n < 3)
	{
		if (hex_ok)
			msg(d, true, "an ASCII frame needs an address, a function code and an LRC (3 bytes), got %u", n);
		w_close(d);
		return;
	}
	{
		u32 expected = modbus_ascii_lrc(bin, n - 1);

		w_num(d, "lrc", bin[n - 1], 2, NULL);
		w_bool(d, "lrc_ok", bin[n - 1] == expected);
		if (bin[n - 1] != expected)
		{
			w_num(d, "lrc_expected", expected, 2, NULL);
			msg(d, true, "LRC mismatch: the frame has 0x%02X, the data gives 0x%02X", bin[n - 1], expected);
		}
	}
	w_close(d);
	d->base = 1;
	emit_pdu(d, bin + 1, n - 2, want);
}

/* ---- framing detection ---- */

static bool
fits_mbap(const u8* p, u32 n)
{
	mbap_header_t h;

	if (n < MBAP_HEADER_SIZE + 1)
		return false;
	mbap_decode(p, &h);
	return mbap_is_valid(&h) &&
	       mbap_frame_size(&h) == n;
}

static bool
fits_rtu(const u8* p, u32 n)
{
	u8		addr;
	const u8*	pdu;
	u32		pdu_len;

	return modbus_rtu_decode(p, n, &addr, &pdu, &pdu_len);
}

static bool
fits_ascii(const u8* p, u32 n)
{
	u8		out[MODBUS_MAX_PDU + 2];
	u8		addr;
	const u8*	pdu;
	u32		pdu_len;

	return modbus_ascii_decode(p, n, out, &addr, &pdu, &pdu_len);
}

static const char*
framing_name(decode_framing_t framing)
{
	return framing == DECODE_FRAMING_MBAP ? "mbap" : framing == DECODE_FRAMING_RTU ? "rtu" : "ascii";
}

/** @brief Writes `bytes`, `framing` and the ADU and PDU of `data` as `framing`. */
static void
emit_frame(dec_t* d, const u8* data, u32 size, decode_framing_t framing, const char* framing_reason,
	   decode_direction_t want)
{
	w_num(d, "bytes", size, 0, NULL);
	w_str(d, "framing", framing_name(framing));
	if (framing_reason)
		w_str(d, "framing_reason", framing_reason);
	d->base = 0;
	switch (framing)
	{
		case DECODE_FRAMING_MBAP:
			if (size < MBAP_HEADER_SIZE)
			{
				cur_t c = { .p = data, .n = size, .pos = 0, .off = 0, .bad = false };

				w_open(d, "adu", false);
				rd(d, &c, "transaction", 2, true, NULL);
				rd(d, &c, "protocol", 2, true, NULL);
				rd(d, &c, "length", 2, true, NULL);
				rd(d, &c, "unit", 1, false, NULL);
				w_close(d);
			}
			else
				emit_mbap(d, data, size, want);
			break;
		case DECODE_FRAMING_RTU:
			emit_rtu(d, data, size, want);
			break;
		default:
			emit_ascii(d, data, size, want);
			break;
	}
	flush_msgs(d);
}

decode_status_t
decode_frame(const u8* data, u32 size, decode_framing_t framing, decode_direction_t direction,
	     bool json, char* out, u32 cap, u32* len)
{
	dec_t		d = { .out = out, .cap = cap, .json = json };
	decode_framing_t found[3];
	u32		count = 0;
	bool		all_fatal = true;
	const char*	reason = NULL;

	if (!out ||
	    cap == 0 ||
	    !len ||
	    (!data && size > 0) ||
	    size > DECODE_MAX_FRAME)
		return DECODE_INVALID;
	out[0] = '\0';
	*len   = 0;
	if (json)
		put(&d, "{");

	if (framing == DECODE_FRAMING_AUTO)
	{
		if (fits_mbap(data, size))
			found[count++] = DECODE_FRAMING_MBAP;
		if (fits_rtu(data, size))
			found[count++] = DECODE_FRAMING_RTU;
		if (fits_ascii(data, size))
			found[count++] = DECODE_FRAMING_ASCII;
		if (count == 1)
		{
			framing = found[0];
			reason  = framing == DECODE_FRAMING_MBAP ? "protocol id is 0 and the length field matches" :
				  framing == DECODE_FRAMING_RTU ? "the CRC matches" :
				  "starts with ':' and the LRC matches";
		}
		else if (count == 0)
		{
			framing = size > 0 && data[0] == ':' ? DECODE_FRAMING_ASCII :
				  (size >= 8 && get_be16(data + 2) == 0 && get_be16(data + 4) >= 2 &&
				   get_be16(data + 4) <= MODBUS_MAX_PDU + 1) ? DECODE_FRAMING_MBAP : DECODE_FRAMING_RTU;
			reason  = "no framing validates; assumed the closest one";
		}
	}
	if (count > 1)
	{
		w_num(&d, "bytes", size, 0, NULL);
		w_bool(&d, "ambiguous", true);
		w_open(&d, "candidates", true);
		for (u32 i = 0; i < count; i++)
		{
			d.any_fatal = false;
			w_open(&d, NULL, false);
			emit_frame(&d, data, size, found[i], "valid in this framing too", direction);
			w_close(&d);
			all_fatal = all_fatal && d.any_fatal;
		}
		w_close(&d);
		d.any_fatal = all_fatal;	/* one candidate that decodes cleanly is enough */
	}
	else
		emit_frame(&d, data, size, framing, reason, direction);

	if (json)
		put(&d, "}");
	if (!d.trunc)
		d.out[d.len] = '\0';
	*len = d.len;
	if (d.trunc)
		return DECODE_TRUNCATED;
	return d.any_fatal ? DECODE_MALFORMED : DECODE_OK;
}
