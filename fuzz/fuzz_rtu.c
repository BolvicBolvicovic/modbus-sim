/**
 * @file
 * @brief Fuzzes RTU framing: the frame codec on raw bytes, and the stream framer used by RTU
 *        byte streams and RTU-over-TCP, fed in fuzzer-chosen chunks with simulated gaps.
 *
 * Input layout for the stream part: repeated [control byte][chunk], where the control byte's low
 * 5 bits give the chunk length (1-32) and bit 7 says "silence after this chunk" (a t3.5 gap).
 */
#include "fuzz_common.h"
#include "modbus/rtu.h"
#include "net/rtu_framer.h"

static void
fuzz_codec(const u8* data, u32 len)
{
	u8		addr = 0;
	const u8*	pdu = NULL;
	u32		pdu_len = 0;
	u32		frame_len = 0;
	u8		out[MODBUS_RTU_MAX_FRAME];

	if (modbus_rtu_decode(data, len, &addr, &pdu, &pdu_len))
	{
		FUZZ_CHECK(pdu_len + 3 == len && pdu == data + 1);
		FUZZ_CHECK(modbus_rtu_encode(out, addr, pdu, pdu_len) == len && memcmp(out, data, len) == 0);
	}
	if (modbus_rtu_next_frame(data, len, &frame_len) == MODBUS_RTU_FRAME_FOUND)
		FUZZ_CHECK(frame_len >= 4 && frame_len <= len);
	if (modbus_rtu_request_len(data, len, &frame_len))
		FUZZ_CHECK(frame_len <= MODBUS_RTU_MAX_FRAME);
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	device_t*		devices[] = { fuzz_device() };
	rtu_framer_t		framer;
	rtu_framer_ctx_t	ctx = {
		.devices           = devices,
		.device_count      = 1,
		.rng               = fuzz_rng(),
		.frame_gap_ms      = 0,
		.baud              = size > 0 && (data[0] & 1) ? 9600 : 0,
		.strict_t15        = size > 0 && (data[0] & 2),
		.broadcast_execute = size > 0 && (data[0] & 4),
	};
	u64			now_ms = 1000;
	size_t			i = 1;

	if (size > 4096)
		return 0;
	fuzz_codec(data, (u32)size);
	rtu_framer_init(&framer);
	while (i < size)
	{
		u8	control = data[i++];
		u32	chunk = (control & 0x1F) + 1;

		if (chunk > size - i)
			chunk = (u32)(size - i);
		if (framer.rx_len >= sizeof(framer.rx))
			rtu_framer_consume(&framer, framer.rx_len - (MODBUS_RTU_MAX_FRAME - 1));
		if (chunk > sizeof(framer.rx) - framer.rx_len)
			chunk = (u32)(sizeof(framer.rx) - framer.rx_len);
		rtu_framer_note_gap(&framer, &ctx, now_ms, now_ms * 1000);
		memcpy(framer.rx + framer.rx_len, data + i, chunk);
		framer.rx_len += chunk;
		framer.have_rx = true;
		i += chunk;
		now_ms += 1;
		rtu_framer_on_new_data(&framer, &ctx, now_ms);
		if (control & 0x80)
		{
			now_ms += 50;
			rtu_framer_on_gap_due(&framer, &ctx, now_ms, now_ms * 1000);
		}
		rtu_framer_process(&framer, &ctx, now_ms);
		FUZZ_CHECK(framer.rx_len <= sizeof(framer.rx));
		if (framer.pending.active)
		{
			FUZZ_CHECK(framer.pending.len >= 4 && framer.pending.len <= MODBUS_RTU_MAX_FRAME);
			pending_response_clear(&framer.pending);	/* as if sent */
		}
	}
	return 0;
}
