#include "modbus/mbap.h"
#include "modbus/bytes.h"

void
mbap_decode(const u8* buf, mbap_header_t* header)
{
	header->transaction = get_be16(buf);
	header->protocol    = get_be16(buf + 2);
	header->length      = get_be16(buf + 4);
	header->unit        = buf[6];
}

void
mbap_encode(u8* buf, const mbap_header_t* header)
{
	put_be16(buf, header->transaction);
	put_be16(buf + 2, header->protocol);
	put_be16(buf + 4, header->length);
	buf[6] = header->unit;
}

bool
mbap_is_valid(const mbap_header_t* header)
{
	if (header->protocol != 0)
		return false;

	return header->length >= 2 && header->length <= MODBUS_MAX_PDU + 1;
}

u32
mbap_frame_size(const mbap_header_t* header)
{
	return 6 + (u32)header->length;
}
