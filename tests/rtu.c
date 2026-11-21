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
