/**
 * @file
 * @brief Fuzzes the PDU handler (every supported function code) with arbitrary requests.
 *
 * Invariants: a response never exceeds MODBUS_MAX_PDU and echoes the function code (with bit 7
 * set for an exception); an exception is exactly two bytes, except the FC 43 / MEI 0x0D CANopen
 * extended exception `AB FF <len-hi> <len-lo> 0D <AE|CE> <payload>`, whose length matches its field.
 */
#include "fuzz_common.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	static u64		now_ms;
	device_t*		dev = fuzz_device();
	u8			req[MODBUS_MAX_PDU];
	u8			resp[MODBUS_MAX_PDU + 16];
	fault_decision_t	decision = { 0 };
	u32			len = size > MODBUS_MAX_PDU ? MODBUS_MAX_PDU : (u32)size;
	u32			resp_len;

	if (len == 0)
		return 0;
	memcpy(req, data, len);	/* exact-size copy so ASan sees reads past the request */
	now_ms += 7;
	resp_len = modbus_handle_request(dev, fuzz_rng(), now_ms, req, len, resp, &decision);
	FUZZ_CHECK(resp_len <= MODBUS_MAX_PDU);
	if (resp_len == 0)
		return 0;
	if (resp[0] & 0x80)
	{
		FUZZ_CHECK(resp[0] == (u8)(req[0] | 0x80));
		if (req[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp_len > 2)
			FUZZ_CHECK(resp_len >= 6 && resp[1] == 0xFF && resp[4] == 0x0D &&
				   (u32)((resp[2] << 8) | resp[3]) + 4 == resp_len);
		else
			FUZZ_CHECK(resp_len == 2);
	}
	else
		FUZZ_CHECK(resp[0] == req[0]);
	return 0;
}
