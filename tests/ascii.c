#include "vtest.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include <string.h>

TEST(AsciiLrcKnownVector)
{
	/* Modbus ASCII spec example: 01 03 00 00 00 0A -> LRC F2. */
	const u8 frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };

	ASSERT_EQ(modbus_ascii_lrc(frame, sizeof(frame)) == 0xF2);
}

TEST(AsciiLrcIsTwosComplementSum)
{
	const u8 frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01 };

	ASSERT_EQ(modbus_ascii_lrc(frame, sizeof(frame)) == 0xFB);
	ASSERT_EQ(modbus_ascii_lrc(frame, 0) == 0);
}

TEST(AsciiEncodeBuildsUppercaseFrame)
{
	const u8 pdu[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	const char expected[] = ":010300000001FB\r\n";
	u8 buf[MODBUS_ASCII_MAX_FRAME];
	u32 len = modbus_ascii_encode(buf, 0x01, pdu, sizeof(pdu));

	ASSERT_EQ(len == sizeof(expected) - 1);
	ASSERT_EQ(memcmp(buf, expected, len) == 0);
}

TEST(AsciiEncodeRejectsEmptyAndOversizedPdu)
{
	u8 pdu[MODBUS_MAX_PDU + 1];
	u8 buf[MODBUS_ASCII_MAX_FRAME + 4];

	memset(pdu, 0, sizeof(pdu));
	ASSERT_EQ(modbus_ascii_encode(buf, 1, pdu, 0) == 0);
	ASSERT_EQ(modbus_ascii_encode(buf, 1, pdu, sizeof(pdu)) == 0);
}

TEST(AsciiDecodeRoundTrip)
{
	const u8 pdu[] = { 0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x0A, 0x01, 0x02 };
	u8 buf[MODBUS_ASCII_MAX_FRAME];
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* out = NULL;
	u32 out_len = 0;
	u32 frame_len = modbus_ascii_encode(buf, 0x11, pdu, sizeof(pdu));

	ASSERT_SUCCESS(modbus_ascii_decode(buf, frame_len, decoded, &addr, &out, &out_len));
	ASSERT_EQ(addr == 0x11);
	ASSERT_EQ(out_len == sizeof(pdu));
	ASSERT_EQ(memcmp(out, pdu, sizeof(pdu)) == 0);
}

TEST(AsciiDecodeAcceptsLowercaseHex)
{
	const char frame[] = ":010300000001fb\r\n";
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	ASSERT_SUCCESS(modbus_ascii_decode((const u8*)frame, sizeof(frame) - 1,
					   decoded, &addr, &pdu, &pdu_len));
	ASSERT_EQ(addr == 1 && pdu_len == 5);
	ASSERT_EQ(pdu[0] == 0x03 && get_be16(pdu + 3) == 1);
}

TEST(AsciiDecodeRejectsBadLrc)
{
	const char frame[] = ":010300000001FA\r\n";
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	ASSERT_FAILURE(modbus_ascii_decode((const u8*)frame, sizeof(frame) - 1,
					   decoded, &addr, &pdu, &pdu_len));
}

TEST(AsciiDecodeRejectsMalformedFrames)
{
	struct
	{
		const char*	name;
		const char*	frame;
	} cases[] = {
		{ "no colon",      "010300000001FB\r\n" },
		{ "no crlf",       ":010300000001FB\rX" },
		{ "wrong order",   ":010300000001FB\n\r" },
		{ "odd hex count", ":01030000001FB\r\n" },
		{ "bad hex digit", ":01G300000001FB\r\n" },
		{ "too short",     ":01FF\r\n" },
	};
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		bool ok = modbus_ascii_decode((const u8*)cases[i].frame,
					      (u32)strlen(cases[i].frame),
					      decoded, &addr, &pdu, &pdu_len);

		ASSERT_FAILURE(ok);
	}
}

TEST(AsciiDecodeRejectsOversizedFrame)
{
	u8 frame[MODBUS_ASCII_MAX_FRAME + 1];
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	memset(frame, '0', sizeof(frame));
	frame[0] = ':';
	frame[sizeof(frame) - 2] = '\r';
	frame[sizeof(frame) - 1] = '\n';
	ASSERT_FAILURE(modbus_ascii_decode(frame, sizeof(frame), decoded,
					  &addr, &pdu, &pdu_len));
}

TEST(AsciiScanFindsCompleteFrameAndPartial)
{
	const char frame[] = ":010300000001FB\r\n";
	u32 len = 0;

	ASSERT_EQ(modbus_ascii_scan((const u8*)frame, sizeof(frame) - 1, &len) ==
		  MODBUS_ASCII_SCAN_FRAME);
	ASSERT_EQ(len == sizeof(frame) - 1);
	ASSERT_EQ(modbus_ascii_scan((const u8*)frame, sizeof(frame) - 3, &len) ==
		  MODBUS_ASCII_SCAN_NONE);
}

TEST(AsciiScanDiscardsGarbageAndBadTerminators)
{
	const char garbage[] = "junk:010300000001FB\r\n";
	const char lone_cr[] = ":0103\r";
	const char lone_lf[] = ":0103\n";
	u32 len = 0;

	ASSERT_EQ(modbus_ascii_scan((const u8*)garbage, sizeof(garbage) - 1, &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(modbus_ascii_scan((const u8*)lone_cr, sizeof(lone_cr) - 1, &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(lone_cr) - 1);
	ASSERT_EQ(modbus_ascii_scan((const u8*)lone_lf, sizeof(lone_lf) - 1, &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(lone_lf) - 1);
}

TEST(AsciiScanDiscardsOversizedFrameAndResyncs)
{
	u8 oversized[MODBUS_ASCII_MAX_FRAME + 4];
	u8 full[MODBUS_ASCII_MAX_FRAME];
	u32 len = 0;

	memset(oversized, '0', sizeof(oversized));
	oversized[0] = ':';
	oversized[sizeof(oversized) - 2] = '\r';
	oversized[sizeof(oversized) - 1] = '\n';
	ASSERT_EQ(modbus_ascii_scan(oversized, sizeof(oversized), &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(oversized));

	memset(full, '0', sizeof(full));
	full[0] = ':';
	full[sizeof(full) - 1] = ':';
	ASSERT_EQ(modbus_ascii_scan(full, sizeof(full), &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(full) - 1);
}
