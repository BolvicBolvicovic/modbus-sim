#ifndef RTU_H
#define RTU_H

#include "vlib/c_types.h"
#include "modbus/mbap.h"
#include <stdbool.h>

/* Modbus RTU frame = 1-byte address + PDU + 2-byte CRC (low byte first). */
#define MODBUS_RTU_MAX_FRAME	(1 + MODBUS_MAX_PDU + 2)

/* CRC-16/Modbus over buf[0..len-1]. Polynomial 0xA001, initial value 0xFFFF. */
u16	modbus_crc16(const u8* buf, u32 len);

/* Name: modbus_rtu_encode
 * Description: builds an RTU frame [addr][pdu...][CRC lo][CRC hi] in `buf`.
 * `buf` must hold at least 1 + pdu_len + 2 bytes. Returns the frame length,
 * or 0 when pdu_len exceeds MODBUS_MAX_PDU.
 * */
u32	modbus_rtu_encode(u8* buf, u8 addr, const u8* pdu, u32 pdu_len);

/* Name: modbus_rtu_decode
 * Description: validates an RTU frame's length and CRC, then returns its
 * address and a pointer to the PDU inside `buf`. On success *pdu points into
 * `buf` and remains valid as long as `buf` is unchanged. Returns false for
 * frames that are too short/long or have a bad CRC.
 * */
bool	modbus_rtu_decode(const u8* buf, u32 len, u8* addr, const u8** pdu, u32* pdu_len);

/* Name: modbus_rtu_request_len
 * Description: derives the expected RTU request frame length from the function
 * code and its length field. `buf` starts with the unit address. Returns false
 * when the header is incomplete, the function code is unsupported, or its
 * length is variable (FC 08 diagnostics), which callers must frame by the
 * inter-frame gap instead.
 * */
bool	modbus_rtu_request_len(const u8* buf, u32 buf_len, u32* frame_len);

#endif
