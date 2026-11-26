#ifndef BYTES_H
#define BYTES_H

#include "vlib/c_types.h"

/**
 * @file
 * @brief Big-endian 16-bit helpers: Modbus is big-endian on the wire.
 */

static inline u16
get_be16(const u8* p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static inline void
put_be16(u8* p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)(v & 0xFF);
}

#endif
