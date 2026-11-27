#include "vtest.h"
#include "modbus/rtu.h"
#include "modbus/pdu.h"
#include "net/rtu_framer.h"
#include <string.h>
#include "vlib/macros.h"

static void
set_corrupt_mode(device_t* dev, fault_corrupt_mode_t mode)
{
	dev->faults.by_kind[FAULT_CORRUPT] = (fault_t){
		.corrupt_mode = mode,
		.percent      = 100,
	};
	dev->faults.present |= FAULT_BIT(FAULT_CORRUPT);
}

TEST(RtuCrcKnownVectors)
{
	static const struct {
		u8	frame[6];
		u16	crc;
	} cases[] = {
		/* Modbus over Serial Line spec, Appendix B example */
		{ { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A }, 0xCDC5 },
		/* common example: CRC 76 87 (lo, hi) */
		{ { 0x11, 0x03, 0x00, 0x6B, 0x00, 0x03 }, 0x8776 },
	};

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
		ASSERT_EQ(modbus_crc16(cases[i].frame, 6) == cases[i].crc);
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

TEST(RtuDecodeRoundTripsFramesAndExceptions)
{
	static const struct {
		u8	addr;
		u8	pdu[10];
		u32	len;
	} cases[] = {
		{ 0x11, { 0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x0A, 0x01, 0x02 }, 10 },
		{ 0x04, { 0x83, 0x02 }, 2 },	/* exception frame */
	};

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		u8	buf[MODBUS_RTU_MAX_FRAME];
		u8	addr    = 0;
		const u8* out   = 0;
		u32	out_len = 0;
		u32	len     = modbus_rtu_encode(buf, cases[i].addr, cases[i].pdu, cases[i].len);

		ASSERT_SUCCESS(modbus_rtu_decode(buf, len, &addr, &out, &out_len));
		ASSERT_EQ(addr == cases[i].addr);
		ASSERT_EQ(out_len == cases[i].len);
		ASSERT_EQ(memcmp(out, cases[i].pdu, cases[i].len) == 0);
	}
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

TEST(RtuChecksumCorruptDamagesCrcButNotPdu)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = device_new(arena, 0);
	point_t			p     = { 0 };
	rtu_framer_t		framer;
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];
	u8			expected[MODBUS_RTU_MAX_FRAME];
	u8			addr = 0;
	const u8*		pdu = NULL;
	u32			resp_len = 0;
	u32			expected_len = 0;
	u32			pdu_len = 0;

	ASSERT_EQ(dev != NULL);
	p.space = SPACE_HOLDING;
	p.address = 0;
	p.type = VT_U16;
	p.kind = POINT_STATIC;
	p.static_value = 0x1234;
	ASSERT_SUCCESS(device_add_point(dev, &p) == NULL);
	set_corrupt_mode(dev, FAULT_CORRUPT_CHECKSUM);
	rng_seed(&rng, 1);

	resp_len = modbus_handle_request(dev, &rng, 0, req, sizeof(req), resp, &decision);
	ASSERT_EQ(resp_len == 4);
	ASSERT_EQ(decision.corrupt == FAULT_CORRUPT_CHECKSUM);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == 0x02);
	ASSERT_EQ(resp[2] == 0x12 && resp[3] == 0x34);

	rtu_framer_init(&framer);
	ASSERT_EQ(rtu_framer_queue(&framer, 1, resp, resp_len, 0,
				   decision.corrupt == FAULT_CORRUPT_CHECKSUM));
	expected_len = modbus_rtu_encode(expected, 1, resp, resp_len);
	ASSERT_EQ(framer.pending.active && framer.pending.len == expected_len);
	ASSERT_EQ(memcmp(framer.pending.buf + 1, resp, resp_len) == 0);
	ASSERT_EQ(memcmp(framer.pending.buf, expected, expected_len - 1) == 0);
	ASSERT_EQ(framer.pending.buf[expected_len - 1] ==
		  (u8)(expected[expected_len - 1] ^ 0xFF));
	ASSERT_FAILURE(modbus_rtu_decode(framer.pending.buf, framer.pending.len,
					 &addr, &pdu, &pdu_len));
	arena_release(arena);
}

TEST(RtuPduCorruptKeepsValidCrcWithWrongData)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = device_new(arena, 0);
	point_t			p     = { 0 };
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];
	u8			frame[MODBUS_RTU_MAX_FRAME];
	u8			addr = 0;
	const u8*		pdu = NULL;
	u32			resp_len = 0;
	u32			frame_len = 0;
	u32			pdu_len = 0;

	ASSERT_EQ(dev != NULL);
	p.space = SPACE_HOLDING;
	p.address = 0;
	p.type = VT_U16;
	p.kind = POINT_STATIC;
	p.static_value = 0x1234;
	ASSERT_SUCCESS(device_add_point(dev, &p) == NULL);
	set_corrupt_mode(dev, FAULT_CORRUPT_PDU);
	rng_seed(&rng, 2);

	resp_len = modbus_handle_request(dev, &rng, 0, req, sizeof(req), resp, &decision);
	ASSERT_EQ(resp_len == 4);
	ASSERT_EQ(decision.corrupt == FAULT_CORRUPT_PDU);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == (0x02 ^ 0xFF));
	ASSERT_EQ(resp[2] == 0x12 && resp[3] == 0x34);

	frame_len = modbus_rtu_encode(frame, 1, resp, resp_len);
	ASSERT_SUCCESS(modbus_rtu_decode(frame, frame_len, &addr, &pdu, &pdu_len));
	ASSERT_EQ(addr == 1 && pdu_len == resp_len);
	ASSERT_EQ(memcmp(pdu, resp, resp_len) == 0);
	arena_release(arena);
}

TEST(RtuRequestLenSupportedFunctionCodes)
{
	static const struct {
		const char*	name;
		u8		pdu[20];
		u32		pdu_len;
		bool		has_length; /* false for FCs framed by gap, e.g. FC 08 */
	} cases[] = {
		{ "fc01", { FC_READ_COILS, 0x00, 0x00, 0x00, 0x01 }, 5, true },
		{ "fc02", { FC_READ_DISCRETE_INPUTS, 0x00, 0x00, 0x00, 0x01 }, 5, true },
		{ "fc03", { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 }, 5, true },
		{ "fc04", { FC_READ_INPUT_REGISTERS, 0x00, 0x00, 0x00, 0x01 }, 5, true },
		{ "fc05", { FC_WRITE_SINGLE_COIL, 0x00, 0x00, 0xFF, 0x00 }, 5, true },
		{ "fc06", { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x2A }, 5, true },
		{ "fc07", { FC_READ_EXCEPTION_STATUS }, 1, true },
		{ "fc08", { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34 }, 5, false },
		{ "fc0b", { FC_GET_COM_EVENT_COUNTER }, 1, true },
		{ "fc0c", { FC_GET_COM_EVENT_LOG }, 1, true },
		{ "fc0f", { FC_WRITE_MULTIPLE_COILS, 0x00, 0x00, 0x00, 0x08,
			    0x01, 0xFF }, 7, true },
		{ "fc10", { FC_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x02,
			    0x04, 0x00, 0x01, 0x00, 0x02 }, 10, true },
		{ "fc11", { FC_REPORT_SERVER_ID }, 1, true },
		{ "fc14", { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01,
			    0x00, 0x00, 0x00, 0x01 }, 9, true },
		{ "fc15", { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01,
			    0x00, 0x00, 0x00, 0x01, 0x00, 0x02 }, 11, true },
		{ "fc16", { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0x00, 0xFF,
			    0x00, 0x00 }, 7, true },
		{ "fc17", { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00,
			    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02,
			    0x00, 0x2A }, 12, true },
		{ "fc18", { FC_READ_FIFO_QUEUE, 0x00, 0x00 }, 3, true },
		{ "fc2b", { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01,
			    0x00 }, 4, true },
		/* MEI 0x0D is variable-length: access flag plus a 3-byte download. */
		{ "fc2b-mei13-canopen", { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D,
					  0x01, 0x00, 0x05, 0x60, 0x41, 0x00,
					  0x00, 0x00, 0x00, 0x03, 0xAA, 0xBB,
					  0xCC }, 15, true },
		/* Optional network id and data type bytes count towards framing. */
		{ "fc2b-mei13-options", { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D,
					  0x06, 0x00, 0x09, 0x01, 0x05, 0x60,
					  0x41, 0x00, 0x00, 0x00, 0x00, 0x02 }, 14, true },
		/* Unsigned24 encoded data is 3 bytes and is rejected by PDU dispatch
		 * with AE after the framer consumes the whole request. */
		{ "fc2b-mei13-unsigned24", { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D,
					    0x07, 0x00, 0x09, 0x02, 0x05, 0x60,
					    0x41, 0x00, 0x00, 0x00, 0x00, 0x01,
					    0x11, 0x22, 0x33 }, 17, true },
	};
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		u32	frame_len = modbus_rtu_encode(frame, 1, cases[i].pdu,
						       cases[i].pdu_len);
		u32	got = 0;

		ASSERT_EQ(frame_len == 1 + cases[i].pdu_len + 2);
		if (cases[i].has_length)
		{
			ASSERT_SUCCESS(modbus_rtu_request_len(frame, frame_len, &got));
			ASSERT_EQ(got == frame_len);

			/* Trailing bytes from a coalesced write must not change the answer. */
			got = 0;
			ASSERT_SUCCESS(modbus_rtu_request_len(frame, frame_len + 3, &got));
			ASSERT_EQ(got == frame_len);
		}
		else
		{
			/* FC 08 Diagnostics has no length field; framed by inter-frame gap. */
			ASSERT_FAILURE(modbus_rtu_request_len(frame, frame_len, &got));
		}
	}
}

TEST(RtuMei13FrameDispatchedAsCanopen)
{
	static const u8	obj[] = { 0x11, 0x22, 0x33, 0x44 };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	const u8	pdu[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x00, 0x00,
			  0x05, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	const u8*	decoded = NULL;
	u8		addr = 0;
	u32		pdu_len = 0;
	u32		frame_len = 0;
	u32		expected = 0;

	ASSERT_EQ(dev != NULL);
	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(obj), obj) == NULL);
	frame_len = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT_EQ(frame_len == 15);
	ASSERT_SUCCESS(modbus_rtu_request_len(frame, frame_len, &expected));
	ASSERT_EQ(expected == frame_len);
	ASSERT_SUCCESS(modbus_rtu_decode(frame, frame_len, &addr, &decoded, &pdu_len));
	ASSERT_EQ(addr == 1 && pdu_len == sizeof(pdu));
	ASSERT_EQ(memcmp(decoded, pdu, sizeof(pdu)) == 0);
	ASSERT_EQ(modbus_handle_pdu(dev, 0, decoded, pdu_len, resp) == 13);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0D);
	ASSERT_EQ(resp[10] == 0x00 && resp[11] == 0x01 && resp[12] == 0x11);
	arena_release(arena);
}

TEST(RtuCoalescedVariableLengthMei13Frames)
{
	static const u8	pdu1[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x01, 0x00,
				  0x05, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02,
				  0xAA, 0xBB };
	static const u8	pdu2[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };
	u8		frame1[MODBUS_RTU_MAX_FRAME];
	u8		frame2[MODBUS_RTU_MAX_FRAME];
	u8		both[MODBUS_RTU_MAX_FRAME * 2];
	const u8*	decoded = NULL;
	u32		f1 = 0;
	u32		f2 = 0;
	u32		got = 0;
	u8		addr = 0;
	u32		pdu_len = 0;

	f1 = modbus_rtu_encode(frame1, 1, pdu1, sizeof(pdu1));
	f2 = modbus_rtu_encode(frame2, 1, pdu2, sizeof(pdu2));
	ASSERT_EQ(f1 == 1 + sizeof(pdu1) + 2 && f2 == 1 + sizeof(pdu2) + 2);
	memcpy(both, frame1, f1);
	memcpy(both + f1, frame2, f2);

	ASSERT_EQ((modbus_rtu_next_frame(both, f1 + f2, &got) == MODBUS_RTU_FRAME_FOUND));
	ASSERT_EQ(got == f1);
	ASSERT_SUCCESS(modbus_rtu_decode(both, got, &addr, &decoded, &pdu_len));
	ASSERT_EQ(addr == 1 && pdu_len == sizeof(pdu1));
	ASSERT_EQ(memcmp(decoded, pdu1, sizeof(pdu1)) == 0);

	ASSERT_EQ((modbus_rtu_next_frame(both + f1, f2, &got) == MODBUS_RTU_FRAME_FOUND));
	ASSERT_EQ(got == f2);
	ASSERT_SUCCESS(modbus_rtu_decode(both + f1, got, &addr, &decoded, &pdu_len));
	ASSERT_EQ(addr == 1 && pdu_len == sizeof(pdu2));
	ASSERT_EQ(memcmp(decoded, pdu2, sizeof(pdu2)) == 0);
}

TEST(RtuRequestLenRejectsUnsupportedAndTruncated)
{
	u8	unknown[] = { 1, 0x2A, 0x00, 0x00, 0x00, 0x00 };
	u8	one_byte[] = { 1 };
	u8	fc0f_short[] = { 1, FC_WRITE_MULTIPLE_COILS, 0x00, 0x00, 0x00, 0x01 };
	u8	fc10_short[] = { 1, FC_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8	fc14_short[] = { 1, FC_READ_FILE_RECORD };
	u8	fc15_short[] = { 1, FC_WRITE_FILE_RECORD };
	u8	mei_short[] = { 1, FC_ENCAPSULATED_INTERFACE_TRANSPORT };
	u8	mei_wrong[] = { 1, FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0F, 0x00 };
	u8	fc0f_oversized[] = { 1, FC_WRITE_MULTIPLE_COILS, 0x00, 0x00,
				     0x00, 0x01, 0xFF };
	u32	len = 0;

	ASSERT_FAILURE(modbus_rtu_request_len(unknown, sizeof(unknown), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(one_byte, sizeof(one_byte), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc0f_short, sizeof(fc0f_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc10_short, sizeof(fc10_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc14_short, sizeof(fc14_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc15_short, sizeof(fc15_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(mei_short, sizeof(mei_short), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(mei_wrong, sizeof(mei_wrong), &len));
	ASSERT_FAILURE(modbus_rtu_request_len(fc0f_oversized, sizeof(fc0f_oversized),
					      &len));
}

TEST(RtuRequestLenFc23ValidAndMalformed)
{
	/* Valid FC 23: read 1 register, write 1 register, byte count 2. */
	u8	valid[] = { 1, FC_READ_WRITE_MULTIPLE_REGISTERS,
			    0x00, 0x00, 0x00, 0x01,
			    0x00, 0x00, 0x00, 0x01,
			    0x02, 0x00, 0x2A, 0x00, 0x00 };
	u32	len = 0;

	ASSERT_SUCCESS(modbus_rtu_request_len(valid, sizeof(valid), &len));
	ASSERT_EQ(len == 1 + 12 + 2); /* addr + pdu + crc */

	/* Boundary: max read qty 0x007D, max write qty 0x0079 with byte count 0xF2. */
	{
		u8	boundary[1 + 10 + 0xF2 + 2];
		u32	i;
		u32	got = 0;

		memset(boundary, 0, sizeof(boundary));
		boundary[0] = 1;
		boundary[1] = FC_READ_WRITE_MULTIPLE_REGISTERS;
		boundary[4] = 0x7D; /* read qty */
		boundary[8] = 0x79; /* write qty */
		boundary[10] = 0xF2; /* byte count */
		for (i = 0; i < sizeof(boundary); i++)
			(void)boundary[i];
		ASSERT_SUCCESS(modbus_rtu_request_len(boundary, sizeof(boundary), &got));
		ASSERT_EQ(got == sizeof(boundary));
	}

	/* Malformed: missing byte count / truncated before byte count field. */
	{
		u8	short_hdr[] = { 1, FC_READ_WRITE_MULTIPLE_REGISTERS,
					0x00, 0x00, 0x00, 0x01,
					0x00, 0x00, 0x00, 0x01 };
		len = 0;
		ASSERT_FAILURE(modbus_rtu_request_len(short_hdr, sizeof(short_hdr), &len));
	}

	/* Malformed: byte count present but payload truncated for declared count. */
	{
		u8	trunc_payload[] = { 1, FC_READ_WRITE_MULTIPLE_REGISTERS,
					    0x00, 0x00, 0x00, 0x01,
					    0x00, 0x00, 0x00, 0x01,
					    0x04, 0x00, 0x01 }; /* needs 4 bytes */
		len = 0;
		/* request_len only computes expected length from byte count; it should
		 * return expected length even if buffer is short? Check behavior: it
		 * does not read beyond byte count field except header; returns computed
		 * frame length. The caller compares to buffer. Here buf_len is short. */
		ASSERT_SUCCESS(modbus_rtu_request_len(trunc_payload, sizeof(trunc_payload), &len));
		ASSERT_EQ(len == 1 + 10 + 4 + 2);
	}

	/* Malformed: byte count makes PDU exceed MODBUS_MAX_PDU. */
	{
		u8	oversized[] = { 1, FC_READ_WRITE_MULTIPLE_REGISTERS,
					0x00, 0x00, 0x00, 0x01,
					0x00, 0x00, 0x00, 0x01,
					0xF5 };
		len = 0;
		ASSERT_FAILURE(modbus_rtu_request_len(oversized, sizeof(oversized), &len));
	}

	/* Malformed: zero/odd-shaped counts in PDU are shape problems for PDU handler;
	 * request_len must still compute length from byte count field when structurally possible. */
	{
		u8	zero_write[] = { 1, FC_READ_WRITE_MULTIPLE_REGISTERS,
					 0x00, 0x00, 0x00, 0x01,
					 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00 };
		len = 0;
		ASSERT_SUCCESS(modbus_rtu_request_len(zero_write, sizeof(zero_write), &len));
		ASSERT_EQ(len == 1 + 10 + 2);
	}
}

TEST(RtuTimingT35T15SpecValues)
{
	/* Modbus over Serial Line spec: t3.5 fixed above 19200, else 3.5 char times. */
	ASSERT_EQ(rtu_t35_us(0) == 0);
	ASSERT_EQ(rtu_t15_us(0) == 0);
	ASSERT_EQ(rtu_t35_us(9600) == 4011); /* ceil(3.5*11/9600*1e6) */
	ASSERT_EQ(rtu_t15_us(9600) == 1719); /* ceil(1.5*11/9600*1e6) */
	ASSERT_EQ(rtu_t35_us(19200) == 2006); /* ceil(3.5*11/19200*1e6) */
	ASSERT_EQ(rtu_t15_us(19200) == 860);
	ASSERT_EQ(rtu_t35_us(19201) == 1750);
	ASSERT_EQ(rtu_t15_us(19201) == 750);
	ASSERT_EQ(rtu_t35_us(115200) == 1750);
	ASSERT_EQ(rtu_t15_us(115200) == 750);
}

TEST(RtuEffectiveGapMsResolution)
{
	/* Explicit override wins for compatibility. */
	ASSERT_EQ(rtu_effective_gap_ms(20, 115200) == 20);
	ASSERT_EQ(rtu_effective_gap_ms(1, 9600) == 1);

	/* Auto mode derives from baud (ceil to whole ms for poll scheduling). */
	ASSERT_EQ(rtu_effective_gap_ms(0, 9600) == 5);
	ASSERT_EQ(rtu_effective_gap_ms(0, 19200) == 3);
	ASSERT_EQ(rtu_effective_gap_ms(0, 115200) == 2);

	/* No override and no baud: conservative PTY/pipe fallback. */
	ASSERT_EQ(rtu_effective_gap_ms(0, 0) == RTU_DEFAULT_FRAME_GAP_MS);
}
