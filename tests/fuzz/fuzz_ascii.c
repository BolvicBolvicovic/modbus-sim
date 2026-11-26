/**
 * @file
 * @brief Fuzzes Modbus ASCII framing: delimiter scanning over a stream and frame decoding, with
 *        an encode/decode round trip for every frame that decodes.
 */
#include "fuzz_common.h"
#include "modbus/ascii.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	u32 pos = 0;

	if (size > 8192)
		return 0;
	while (pos < size)
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(data + pos, (u32)(size - pos), &len);
		u8			decoded[MODBUS_MAX_PDU + 2];
		u8			again[MODBUS_MAX_PDU + 2];
		u8			frame[MODBUS_ASCII_MAX_FRAME];
		u8			addr = 0;
		u8			addr2 = 0;
		const u8*		pdu = NULL;
		const u8*		pdu2 = NULL;
		u32			pdu_len = 0;
		u32			pdu2_len = 0;
		u32			frame_len;

		if (scan == MODBUS_ASCII_SCAN_NONE || len == 0)
			break;
		FUZZ_CHECK(len <= size - pos);
		if (scan == MODBUS_ASCII_SCAN_FRAME &&
		    modbus_ascii_decode(data + pos, len, decoded, &addr, &pdu, &pdu_len))
		{
			FUZZ_CHECK(pdu_len <= MODBUS_MAX_PDU);
			frame_len = modbus_ascii_encode(frame, addr, pdu, pdu_len);
			FUZZ_CHECK(frame_len > 0 && frame_len <= sizeof(frame));
			FUZZ_CHECK(modbus_ascii_decode(frame, frame_len, again, &addr2, &pdu2, &pdu2_len));
			FUZZ_CHECK(addr2 == addr && pdu2_len == pdu_len && memcmp(pdu2, pdu, pdu_len) == 0);
		}
		pos += len;
	}
	return 0;
}
