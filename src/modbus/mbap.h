#ifndef MBAP_H
#define MBAP_H

#include "vlib/c_types.h"
#include <stdbool.h>

/* Modbus TCP frame = 7-byte MBAP header + PDU.
 * The header's `length` counts the unit id byte plus the PDU. */
#define MBAP_HEADER_SIZE	7
#define MODBUS_MAX_PDU		253
#define MBAP_MAX_FRAME		(MBAP_HEADER_SIZE + MODBUS_MAX_PDU)

typedef struct mbap_header_t mbap_header_t;
struct mbap_header_t
{
	u16	transaction;
	u16	protocol;	/* always 0 for Modbus */
	u16	length;
	u8	unit;
};

void	mbap_decode(const u8* buf, mbap_header_t* header);
void	mbap_encode(u8* buf, const mbap_header_t* header);

/* A header is valid when protocol is 0 and there is room for at least a function code. */
bool	mbap_is_valid(const mbap_header_t* header);

/* Total bytes of the frame this header announces (header + PDU). */
u32	mbap_frame_size(const mbap_header_t* header);

#endif
