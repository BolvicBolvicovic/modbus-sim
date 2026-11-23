#ifndef ASCII_H
#define ASCII_H

#include "vlib/c_types.h"
#include "modbus/mbap.h"
#include <stdbool.h>

/* Modbus ASCII frame on the wire: ':' + 2 hex digits per binary byte + CRLF.
 * The binary bytes are [addr][PDU...][LRC], so the maximum is
 * 1 (colon) + 2 (addr) + 2*MODBUS_MAX_PDU (PDU) + 2 (LRC) + 2 (CRLF). */
#define MODBUS_ASCII_MAX_FRAME	(1 + 2 + 2 * MODBUS_MAX_PDU + 2 + 2)

/* LRC = two's complement modulo 256 of the sum of addr..PDU bytes.
 * `buf` holds `len` binary bytes; the LRC byte itself is not included. */
u8	modbus_ascii_lrc(const u8* buf, u32 len);

/* Name: modbus_ascii_encode
 * Description: builds an uppercase ASCII frame
 * ':' [addr hex] [PDU hex] [LRC hex] CR LF in `buf`, which must hold
 * MODBUS_ASCII_MAX_FRAME bytes. Returns the frame length, or 0 when pdu_len
 * is 0 or exceeds MODBUS_MAX_PDU.
 * */
u32	modbus_ascii_encode(u8* buf, u8 addr, const u8* pdu, u32 pdu_len);

/* Name: modbus_ascii_decode
 * Description: validates a complete ASCII frame and decodes its hex digits
 * into `out`, which must hold MODBUS_MAX_PDU + 2 bytes and must not alias
 * `frame`. Accepts upper- and lowercase hex, requires a leading colon and a
 * CRLF terminator, an even hex count, and a valid LRC. On success *pdu points
 * into `out` and is valid as long as `out` is unchanged.
 * */
bool	modbus_ascii_decode(const u8* frame, u32 len, u8* out, u8* addr,
			    const u8** pdu, u32* pdu_len);

typedef enum modbus_ascii_scan_t modbus_ascii_scan_t;
enum modbus_ascii_scan_t
{
	MODBUS_ASCII_SCAN_NONE,    /* No complete frame at rx[0]; keep buffered bytes. */
	MODBUS_ASCII_SCAN_FRAME,   /* Complete frame of *len bytes at rx[0]. */
	MODBUS_ASCII_SCAN_DISCARD, /* Malformed/oversized prefix of *len bytes; drop it. */
};

/* Name: modbus_ascii_scan
 * Description: locates the next deliverable ASCII frame at rx[0]. Bytes before
 * a leading colon, and a partial frame up to the next colon, are reported as
 * DISCARD so the caller can resynchronize from that colon. A lone CR/LF inside
 * a frame is reported as DISCARD through that byte. An oversized frame with or
 * without a terminator is discarded in full.
 * */
modbus_ascii_scan_t	modbus_ascii_scan(const u8* rx, u32 rx_len, u32* len);

#endif
