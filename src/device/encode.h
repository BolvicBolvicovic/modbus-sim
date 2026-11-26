/**
 * @file
 * @brief Value types and their register encoding.
 */
#ifndef ENCODE_H
#define ENCODE_H

#include "vlib/c_types.h"

typedef enum value_type_t value_type_t;
typedef enum word_order_t word_order_t;
typedef enum byte_order_t byte_order_t;

/**
 * @brief How a number is stored in 16-bit Modbus registers.
 *
 * VT_BOOL is for coils and discrete inputs only. VT_STRING is a point of `length` registers
 * (see point_t) holding two ASCII bytes each; it has no numeric value. The values are part of
 * the library ABI (mogbus_value_type_t): append, never reorder.
 */
enum value_type_t
{
	VT_U16,
	VT_S16,
	VT_U32,
	VT_S32,
	VT_F32,
	VT_BOOL,
	VT_U64,
	VT_S64,
	VT_F64,
	VT_STRING,
};

/** @brief For 32- and 64-bit values, whether the most significant register comes first. */
enum word_order_t
{
	WORDS_HIGH_FIRST,
	WORDS_LOW_FIRST,
};

/** @brief Whether the two bytes inside every register of a point are swapped. */
enum byte_order_t
{
	BYTES_BIG,
	BYTES_LITTLE,
};

/** @brief Number of 16-bit registers (or bits) a value of this type occupies; 0 for VT_STRING. */
u32	value_width(value_type_t type);

/**
 * @brief Writes the encoded value into out[0..width-1] and returns the width.
 *
 * Integer/bool types clamp to range, round fractions, and treat NaN as 0. float32 and float64
 * preserve NaN and infinity; finite values outside float32 range map to +/-infinity. Non-finite
 * static values are only accepted for float types in config. 64-bit integers go through f64, so
 * magnitudes above 2^53 lose precision; use encode_bits() for an exact value.
 */
u32	encode_value(value_type_t type, word_order_t order, byte_order_t bytes, f64 value, u16 out[4]);

/** @brief Inverse of encode_value(): decodes the first value of a point from raw registers. */
f64	decode_value(value_type_t type, word_order_t order, byte_order_t bytes, const u16* words);

/**
 * @brief Lays the low width*16 bits of `raw` out in `width` (1-4) registers: most significant
 *        register first or last by `order`, then the bytes of every register swapped by `bytes`.
 */
void	encode_bits(u32 width, word_order_t order, byte_order_t bytes, u64 raw, u16* out);

/** @brief Inverse of encode_bits(). */
u64	decode_bits(u32 width, word_order_t order, byte_order_t bytes, const u16* words);

/**
 * @brief Packs 2*regs bytes into regs registers, first byte in the high byte of the first
 *        register (swapped inside every register for BYTES_LITTLE).
 */
void	encode_string(byte_order_t bytes, const u8* text, u32 regs, u16* out);

/** @brief Inverse of encode_string(): writes 2*regs bytes. */
void	decode_string(byte_order_t bytes, const u16* words, u32 regs, u8* out);

#endif
