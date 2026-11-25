#include "device/encode.h"
#include "vlib/macros.h"
#include <float.h>
#include <math.h>
#include <string.h>

u32
value_width(value_type_t type)
{
	switch (type)
	{
	case VT_U32:
	case VT_S32:
	case VT_F32:
		return 2;
	default:
		return 1;
	}
}

static void
split_u32(word_order_t order, u32 raw, u16 out[2])
{
	u16	high = (u16)(raw >> 16);
	u16	low  = (u16)(raw & 0xFFFF);

	out[0] = (order == WORDS_HIGH_FIRST) ? high : low;
	out[1] = (order == WORDS_HIGH_FIRST) ? low  : high;
}

static u32
join_u32(word_order_t order, const u16* words)
{
	u16	high = (order == WORDS_HIGH_FIRST) ? words[0] : words[1];
	u16	low  = (order == WORDS_HIGH_FIRST) ? words[1] : words[0];

	return ((u32)high << 16) | low;
}

static f64
clamp_round(f64 v, f64 lo, f64 hi)
{
	return round(MIN(hi, MAX(lo, v)));
}

f64
decode_value(value_type_t type, word_order_t order, const u16* words)
{
	u32	raw  = 0;
	f32	real = 0;

	switch (type)
	{
	case VT_BOOL:
		return words[0] != 0 ? 1.0 : 0.0;
	case VT_U16:
		return words[0];
	case VT_S16:
		return (s16)words[0];
	case VT_U32:
		return join_u32(order, words);
	case VT_S32:
		return (s32)join_u32(order, words);
	case VT_F32:
		raw = join_u32(order, words);
		memcpy(&real, &raw, sizeof(real));
		return real;
	}
	return 0;
}

u32
encode_value(value_type_t type, word_order_t order, f64 value, u16 out[2])
{
	u32	raw  = 0;
	f32	real = 0;

	switch (type)
	{
	case VT_BOOL:
		out[0] = (value != 0.0 && value == value);
		return 1;
	case VT_U16:
		if (value != value)
			value = 0;
		out[0] = (u16)clamp_round(value, 0, 65535);
		return 1;
	case VT_S16:
		if (value != value)
			value = 0;
		out[0] = (u16)(s16)clamp_round(value, -32768, 32767);
		return 1;
	case VT_U32:
		if (value != value)
			value = 0;
		split_u32(order, (u32)clamp_round(value, 0, 4294967295.0), out);
		return 2;
	case VT_S32:
		if (value != value)
			value = 0;
		split_u32(order, (u32)(s32)clamp_round(value, -2147483648.0, 2147483647.0), out);
		return 2;
	case VT_F32:
		if (value != value)
			real = NAN;
		else if (isinf(value))
			real = value < 0 ? -INFINITY : INFINITY;
		else if (value > (f64)FLT_MAX)
			real = INFINITY;
		else if (value < -(f64)FLT_MAX)
			real = -INFINITY;
		else
			real = (f32)value;
		memcpy(&raw, &real, sizeof(raw));
		split_u32(order, raw, out);
		return 2;
	}
	return 0;
}
