/**
 * @file
 * @brief Value types and their register encoding.
 */
#ifndef ENCODE_H
#define ENCODE_H

#include "vlib/c_types.h"

/**
 * @brief How a number is stored in 16-bit Modbus registers.
 *
 * VT_BOOL is for coils and discrete inputs only. Keep the order: the config parser maps names
 * to these values by index.
 */
typedef enum value_type_t value_type_t;
enum value_type_t
{
	VT_U16,
	VT_S16,
	VT_U32,
	VT_S32,
	VT_F32,
	VT_BOOL,
};

/** @brief For 32-bit values, which register holds the high half. */
typedef enum word_order_t word_order_t;
enum word_order_t
{
	WORDS_HIGH_FIRST,
	WORDS_LOW_FIRST,
};

/** @brief Number of 16-bit registers (or bits) a value of this type occupies. */
u32	value_width(value_type_t type);

/**
 * @brief Writes the encoded value into out[0..width-1] and returns the width.
 *
 * Integer/bool types clamp to range, round fractions, and treat NaN as 0. float32 preserves
 * NaN and infinity; finite values outside float range map to +/-infinity. Non-finite static
 * values are only accepted for float32 in config.
 */
u32	encode_value(value_type_t type, word_order_t order, f64 value, u16 out[2]);

#endif
