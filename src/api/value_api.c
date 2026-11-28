/**
 * @file
 * @brief mogbus_registers_to_value() and friends: the register encoding of the points, public.
 *
 * They reuse `device/encode.c`, so a value read or written here is laid out exactly as a point
 * of the same type and orders. No context, no lock, no error text.
 */
#include "mogbus.h"
#include "device/encode.h"
#include <string.h>

/** @brief Most registers a text value takes (the string point limit). */
enum
{
	TEXT_MAX_REGS = 123,
};

/* Numeric types only: BOOL is a bit and STRING has its own calls. */
static bool
numeric(mogbus_value_type_t type)
{
	return (unsigned)type <= MOGBUS_TYPE_F32 ||
	       ((unsigned)type >= MOGBUS_TYPE_U64 && (unsigned)type <= MOGBUS_TYPE_F64);
}

static bool
orders_valid(mogbus_word_order_t word_order, mogbus_byte_order_t byte_order)
{
	return (unsigned)word_order <= MOGBUS_WORDS_LOW_FIRST &&
	       (unsigned)byte_order <= MOGBUS_BYTES_LITTLE;
}

mogbus_result_t
mogbus_registers_to_value(const uint16_t* regs, mogbus_value_type_t type, mogbus_word_order_t word_order,
			  mogbus_byte_order_t byte_order, double* out)
{
	if (!regs ||
	    !out ||
	    !numeric(type) ||
	    !orders_valid(word_order, byte_order))
		return MOGBUS_ERR_INVALID;
	*out = decode_value((value_type_t)type, (word_order_t)word_order, (byte_order_t)byte_order, regs);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_value_to_registers(double value, mogbus_value_type_t type, mogbus_word_order_t word_order,
			  mogbus_byte_order_t byte_order, uint16_t* regs, size_t* count)
{
	u16 words[4];
	u32 width;

	if (!regs ||
	    !numeric(type) ||
	    !orders_valid(word_order, byte_order))
		return MOGBUS_ERR_INVALID;
	width = encode_value((value_type_t)type, (word_order_t)word_order, (byte_order_t)byte_order, value, words);
	memcpy(regs, words, width * sizeof(words[0]));
	if (count)
		*count = width;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_registers_to_u64(const uint16_t* regs, mogbus_word_order_t word_order, mogbus_byte_order_t byte_order,
			uint64_t* out)
{
	if (!regs ||
	    !out ||
	    !orders_valid(word_order, byte_order))
		return MOGBUS_ERR_INVALID;
	*out = decode_bits(4, (word_order_t)word_order, (byte_order_t)byte_order, regs);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_u64_to_registers(uint64_t value, mogbus_word_order_t word_order, mogbus_byte_order_t byte_order,
			uint16_t* regs)
{
	if (!regs ||
	    !orders_valid(word_order, byte_order))
		return MOGBUS_ERR_INVALID;
	encode_bits(4, (word_order_t)word_order, (byte_order_t)byte_order, value, regs);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_registers_to_text(const uint16_t* regs, size_t count, mogbus_byte_order_t byte_order, char* out,
			 size_t cap)
{
	u8	bytes[2 * TEXT_MAX_REGS];
	size_t	len;

	if (!regs ||
	    !out ||
	    cap == 0 ||
	    count > TEXT_MAX_REGS ||
	    (unsigned)byte_order > MOGBUS_BYTES_LITTLE)
		return MOGBUS_ERR_INVALID;
	decode_string((byte_order_t)byte_order, regs, (u32)count, bytes);
	len = 0;
	while (len < 2 * count &&
	       bytes[len] != 0)
		len++;
	if (len >= cap)
	{
		memcpy(out, bytes, cap - 1);
		out[cap - 1] = '\0';
		return MOGBUS_ERR_NOMEM;
	}
	memcpy(out, bytes, len);
	out[len] = '\0';
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_text_to_registers(const char* text, size_t count, mogbus_byte_order_t byte_order, uint16_t* regs)
{
	u8	bytes[2 * TEXT_MAX_REGS] = { 0 };
	size_t	len;

	if (!text ||
	    !regs ||
	    count == 0 ||
	    count > TEXT_MAX_REGS ||
	    (unsigned)byte_order > MOGBUS_BYTES_LITTLE)
		return MOGBUS_ERR_INVALID;
	len = strlen(text);
	if (len > 2 * count)
		return MOGBUS_ERR_INVALID;
	memcpy(bytes, text, len);
	encode_string((byte_order_t)byte_order, bytes, (u32)count, regs);
	return MOGBUS_OK;
}
