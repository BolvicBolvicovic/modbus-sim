#include "vtest.h"
#include "modbus/rtu.h"
#include "modbus/pdu.h"
#include <string.h>

TEST(RtuCrcKnownVector)
{
	/* Modbus over Serial Line spec, Appendix B example: 01 03 00 00 00 0A */
	const u8 frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };

	ASSERT_EQ(modbus_crc16(frame, sizeof(frame)) == 0xCDC5);
}

TEST(RtuCrcAnotherVector)
{
	/* Common Modbus RTU example: 11 03 00 6B 00 03 -> CRC 76 87 (lo, hi) */
	const u8 frame[] = { 0x11, 0x03, 0x00, 0x6B, 0x00, 0x03 };

	ASSERT_EQ(modbus_crc16(frame, sizeof(frame)) == 0x8776);
}

TEST(RtuEncodeBuildsFrame)
{
	const u8 pdu[] = { 0x03, 0x00, 0x00, 0x00, 0x0A };
	u8       buf[MODBUS_RTU_MAX_FRAME];
	u32      len = 0;

	len = modbus_rtu_encode(buf, 0x01, pdu, sizeof(pdu));
	ASSERT_EQ(len == 8);
	ASSERT_EQ(buf[0] == 0x01);
	ASSERT_EQ(memcmp(buf + 1, pdu, sizeof(pdu)) == 0);
	ASSERT_EQ(buf[6] == 0xC5);
	ASSERT_EQ(buf[7] == 0xCD);
}

TEST(RtuDecodeRoundTrip)
{
	const u8 pdu[] = { 0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x0A, 0x01, 0x02 };
	u8       buf[MODBUS_RTU_MAX_FRAME];
	u8       addr = 0;
	const u8 *out = 0;
	u32      len = 0;
	u32      out_len = 0;

	len = modbus_rtu_encode(buf, 0x11, pdu, sizeof(pdu));
	ASSERT_SUCCESS(modbus_rtu_decode(buf, len, &addr, &out, &out_len));
	ASSERT_EQ(addr == 0x11);
	ASSERT_EQ(out_len == sizeof(pdu));
	ASSERT_EQ(memcmp(out, pdu, sizeof(pdu)) == 0);
}

TEST(RtuDecodeRejectsBadCrc)
{
	const u8 pdu[] = { 0x03, 0x00, 0x00, 0x00, 0x0A };
	u8       buf[MODBUS_RTU_MAX_FRAME];
	u8       addr = 0;
	const u8 *out = 0;
	u32      len = 0;
	u32      out_len = 0;

	len = modbus_rtu_encode(buf, 0x01, pdu, sizeof(pdu));
	buf[len - 1] ^= 0x01;
	ASSERT_FAILURE(modbus_rtu_decode(buf, len, &addr, &out, &out_len));
}

TEST(RtuDecodeRejectsBadLength)
{
	u8       buf[MODBUS_RTU_MAX_FRAME + 1];
	u8       addr = 0;
	const u8 *out = 0;
	u32      out_len = 0;

	memset(buf, 0, sizeof(buf));
	ASSERT_FAILURE(modbus_rtu_decode(buf, 3, &addr, &out, &out_len));
	ASSERT_FAILURE(modbus_rtu_decode(buf, MODBUS_RTU_MAX_FRAME + 1, &addr, &out, &out_len));
}

TEST(RtuEncodeRejectsOversizedPdu)
{
	u8 pdu[MODBUS_MAX_PDU + 1];
	u8 buf[MODBUS_RTU_MAX_FRAME + 4];

	memset(pdu, 0, sizeof(pdu));
	ASSERT_EQ(modbus_rtu_encode(buf, 1, pdu, sizeof(pdu)) == 0);
}

TEST(RtuDecodeExceptionFrame)
{
	const u8 pdu[] = { 0x83, 0x02 };
	u8       buf[MODBUS_RTU_MAX_FRAME];
	u8       addr = 0;
	const u8 *out = 0;
	u32      len = 0;
	u32      out_len = 0;

	len = modbus_rtu_encode(buf, 0x04, pdu, sizeof(pdu));
	ASSERT_SUCCESS(modbus_rtu_decode(buf, len, &addr, &out, &out_len));
	ASSERT_EQ(addr == 0x04);
	ASSERT_EQ(out_len == 2);
	ASSERT_EQ(out[0] == 0x83 && out[1] == 0x02);
}

TEST(RtuRequestLenMatchesFunctionCode)
{
	static const struct {
		const char*	name;
		u8		pdu[16];
		u32		pdu_len;
	} cases[] = {
		{ "fc01", { FC_READ_COILS, 0x00, 0x00, 0x00, 0x01 }, 5 },
		{ "fc02", { FC_READ_DISCRETE_INPUTS, 0x00, 0x00, 0x00, 0x01 }, 5 },
		{ "fc03", { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 }, 5 },
		{ "fc04", { FC_READ_INPUT_REGISTERS, 0x00, 0x00, 0x00, 0x01 }, 5 },
		{ "fc05", { FC_WRITE_SINGLE_COIL, 0x00, 0x00, 0xFF, 0x00 }, 5 },
		{ "fc06", { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x2A }, 5 },
		{ "fc07", { FC_READ_EXCEPTION_STATUS }, 1 },
		{ "fc0b", { FC_GET_COM_EVENT_COUNTER }, 1 },
		{ "fc0c", { FC_GET_COM_EVENT_LOG }, 1 },
		{ "fc0f", { FC_WRITE_MULTIPLE_COILS, 0x00, 0x00, 0x00, 0x08,
			    0x01, 0xFF }, 7 },
		{ "fc10", { FC_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x02,
			    0x04, 0x00, 0x01, 0x00, 0x02 }, 10 },
		{ "fc11", { FC_REPORT_SERVER_ID }, 1 },
		{ "fc14", { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01,
			    0x00, 0x00, 0x00, 0x01 }, 9 },
		{ "fc15", { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01,
			    0x00, 0x00, 0x00, 0x01, 0x00, 0x02 }, 11 },
		{ "fc16", { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0x00, 0xFF,
			    0x00, 0x00 }, 7 },
		{ "fc17", { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00,
			    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02,
			    0x00, 0x2A }, 12 },
		{ "fc18", { FC_READ_FIFO_QUEUE, 0x00, 0x00 }, 3 },
		{ "fc2b", { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01,
			    0x00 }, 4 },
	};
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		u32	frame_len = modbus_rtu_encode(frame, 1, cases[i].pdu,
						       cases[i].pdu_len);
		u32	got = 0;

		ASSERT_EQ(frame_len == 1 + cases[i].pdu_len + 2);
		ASSERT_SUCCESS(modbus_rtu_request_len(frame, frame_len, &got));
		ASSERT_EQ(got == frame_len);

		/* Trailing bytes from a coalesced write must not change the answer. */
		got = 0;
		ASSERT_SUCCESS(modbus_rtu_request_len(frame, frame_len + 3, &got));
		ASSERT_EQ(got == frame_len);
	}
}

TEST(RtuRequestLenRejectsVariableUnsupportedAndTruncated)
{
	u8	fc08[] = { 1, FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34, 0x00, 0x00 };
	u8	unknown[] = { 1, 0x2A, 0x00, 0x00, 0x00, 0x00 };
	u8	one_byte[] = { 1 };
	u8	fc0f_short[] = { 1, FC_WRITE_MULTIPLE_COILS, 0x00, 0x00, 0x00, 0x01 };
	u8	fc17_short[] = { 1, FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00,
				 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8	fc14_short[] = { 1, FC_READ_FILE_RECORD };
	u8	mei_short[] = { 1, FC_ENCAPSULATED_INTERFACE_TRANSPORT };
	u8	mei_wrong[] = { 1, FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x00 };
	u8	fc0f_oversized[] = { 1, FC_WRITE_MULTIPLE_COILS, 0x00, 0x00,
				     0x00, 0x01, 0xFF };
	u32	len = 0;

	/* Diagnostics has no length field; it is framed by the inter-frame gap. */
	ASSERT_FAILURE(modbus_rtu_request_len(fc08, sizeof(fc08), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(unknown, sizeof(unknown), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(one_byte, sizeof(one_byte), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc0f_short, sizeof(fc0f_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc17_short, sizeof(fc17_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc14_short, sizeof(fc14_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(mei_short, sizeof(mei_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(mei_wrong, sizeof(mei_wrong), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc0f_oversized, sizeof(fc0f_oversized),
					      &len));
}
