/**
 * @file
 * @brief Encoding and decoding of point values to and from 16-bit registers.
 */
#include "device/encode.h"
#include "vlib/macros.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
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
	case VT_U64:
	case VT_S64:
	case VT_F64:
		return 4;
	case VT_STRING:
		return 0;
	default:
		return 1;
	}
}

static u16
swap_bytes(byte_order_t bytes, u16 word)
{
	return bytes == BYTES_LITTLE ? (u16)((word >> 8) | (word << 8)) : word;
}

void
encode_bits(u32 width, word_order_t order, byte_order_t bytes, u64 raw, u16* out)
{
	for (u32 i = 0; i < width; i++)
	{
		u16	word = (u16)(raw >> (16 * (width - 1 - i)));

		out[order == WORDS_HIGH_FIRST ? i : width - 1 - i] = swap_bytes(bytes, word);
	}
}

u64
decode_bits(u32 width, word_order_t order, byte_order_t bytes, const u16* words)
{
	u64	raw = 0;

	for (u32 i = 0; i < width; i++)
		raw = (raw << 16) | swap_bytes(bytes, words[order == WORDS_HIGH_FIRST ? i : width - 1 - i]);
	return raw;
}

void
encode_string(byte_order_t bytes, const u8* text, u32 regs, u16* out)
{
	for (u32 i = 0; i < regs; i++)
		out[i] = swap_bytes(bytes, (u16)((text[2 * i] << 8) | text[2 * i + 1]));
}

void
decode_string(byte_order_t bytes, const u16* words, u32 regs, u8* out)
{
	for (u32 i = 0; i < regs; i++)
	{
		u16	word = swap_bytes(bytes, words[i]);

		out[2 * i]     = (u8)(word >> 8);
		out[2 * i + 1] = (u8)word;
	}
}

static f64
clamp_round(f64 v, f64 lo, f64 hi)
{
	return round(MIN(hi, MAX(lo, v)));
}

f64
decode_value(value_type_t type, word_order_t order, byte_order_t bytes, const u16* words)
{
	u64	raw  = 0;
	f32	real = 0;
	f64	wide = 0;
	u32	real_bits = 0;
	u32	width = value_width(type);

	if (type == VT_BOOL)
		return words[0] != 0 ? 1.0 : 0.0;
	if (width == 0)
		return 0;

	raw = decode_bits(width, order, bytes, words);
	switch (type)
	{
	case VT_U16:
		return (u16)raw;
	case VT_S16:
		return (s16)raw;
	case VT_U32:
		return (u32)raw;
	case VT_S32:
		return (s32)raw;
	case VT_F32:
		real_bits = (u32)raw;
		memcpy(&real, &real_bits, sizeof(real));
		return real;
	case VT_U64:
		return (f64)raw;
	case VT_S64:
		return (f64)(s64)raw;
	case VT_F64:
		memcpy(&wide, &raw, sizeof(wide));
		return wide;
	default:
		return 0;
	}
}

u32
encode_value(value_type_t type, word_order_t order, byte_order_t bytes, f64 value, u16 out[4])
{
	u64	raw   = 0;
	u32	width = value_width(type);
	f32	real  = 0;
	u32	real_bits = 0;

	if (type == VT_BOOL)
	{
		out[0] = (value != 0.0 && value == value);
		return 1;
	}
	if (width == 0)
		return 0;

	if (value != value &&
	    type != VT_F32 &&
	    type != VT_F64)
		value = 0;

	switch (type)
	{
	case VT_U16:
		raw = (u16)clamp_round(value, 0, 65535);
		break;
	case VT_S16:
		raw = (u16)(s16)clamp_round(value, -32768, 32767);
		break;
	case VT_U32:
		raw = (u32)clamp_round(value, 0, 4294967295.0);
		break;
	case VT_S32:
		raw = (u32)(s32)clamp_round(value, -2147483648.0, 2147483647.0);
		break;
	case VT_U64:
		raw = value >= 0x1p64 ? UINT64_MAX : value <= 0 ? 0 : (u64)round(value);
		break;
	case VT_S64:
		raw = value >= 0x1p63 ? (u64)INT64_MAX :
		      value <= -0x1p63 ? (u64)INT64_MIN :
		      (u64)(s64)round(value);
		break;
	case VT_F64:
		memcpy(&raw, &value, sizeof(raw));
		break;
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
		memcpy(&real_bits, &real, sizeof(real_bits));
		raw = real_bits;
		break;
	default:
		return 0;
	}
	encode_bits(width, order, bytes, raw, out);
	return width;
}
