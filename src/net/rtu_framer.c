#include "net/rtu_framer.h"
#include "modbus/pdu.h"
#include "net/net_common.h"
#include "util/log.h"
#include <string.h>

void
rtu_pending_clear(rtu_pending_t* pending)
{
	pending->active = false;
	pending->len    = 0;
	pending->sent   = 0;
	pending->due_ms = 0;
}

void
rtu_framer_init(rtu_framer_t* framer)
{
	memset(framer, 0, sizeof(*framer));
	rtu_pending_clear(&framer->pending);
}

static bool
rtu_framer_keep_incomplete(u32 frame_gap_ms, u32 baud)
{
	/* In baud-derived auto mode, read() boundaries do not reflect UART timing,
	 * so keep incomplete bytes across a t3.5 gap. The caller bounds this by
	 * deferring at most one extra gap window. */
	return frame_gap_ms == 0 && baud != 0;
}

void
rtu_framer_consume(rtu_framer_t* framer, u32 n)
{
	if (n >= framer->rx_len)
	{
		framer->rx_len = 0;
		return;
	}
	memmove(framer->rx, framer->rx + n, framer->rx_len - n);
	framer->rx_len -= n;
}

bool
rtu_framer_queue(rtu_framer_t* framer, u8 addr, const u8* pdu, u32 pdu_len, u64 due_ms)
{
	u32 len = modbus_rtu_encode(framer->pending.buf, addr, pdu, pdu_len);

	if (len == 0)
		return false;
	framer->pending.active = true;
	framer->pending.len = len;
	framer->pending.sent = 0;
	framer->pending.due_ms = due_ms;
	return true;
}

static void
handle_frame(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms,
	     const u8* frame, u32 len)
{
	u8			addr = 0;
	const u8*		pdu = NULL;
	u32			pdu_len = 0;
	u8			resp[MODBUS_MAX_PDU];
	u32			resp_len = 0;
	device_t*		dev = NULL;
	fault_decision_t	decision = { 0 };

	if (!modbus_rtu_decode(frame, len, &addr, &pdu, &pdu_len))
		return;
	if (addr == 0)
		return; /* broadcast: no response */
	dev = net_find_device(ctx->devices, ctx->device_count, addr);
	if (!dev)
		return; /* unknown unit: no response */

	resp_len = net_handle_pdu(dev, ctx->rng, now_ms, pdu, pdu_len, resp, &decision);
	if (ctx->peer)
		LOG_DEBUG("%s rtu unit=%u fc=0x%02X -> %s%s", ctx->peer, addr, pdu[0],
			  (resp[0] & 0x80) ? "exception" : "ok",
			  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");
	else
		LOG_DEBUG("rtu unit=%u fc=0x%02X -> %s%s", addr, pdu[0],
			  (resp[0] & 0x80) ? "exception" : "ok",
			  decision.drop ? " (dropped by fault)" : decision.delay_ms ? " (delayed by fault)" : "");
	if (decision.drop)
		return;
	(void)rtu_framer_queue(framer, addr, resp, resp_len, now_ms + decision.delay_ms);
}

bool
rtu_framer_process(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms)
{
	bool keep_incomplete = rtu_framer_keep_incomplete(ctx->frame_gap_ms, ctx->baud);

	while (framer->rx_len > 0 && !framer->pending.active)
	{
		u32				frame_len = 0;
		modbus_rtu_frame_result_t	result = modbus_rtu_next_frame(framer->rx, framer->rx_len, &frame_len);

		if (result == MODBUS_RTU_FRAME_FOUND)
		{
			handle_frame(framer, ctx, now_ms, framer->rx, frame_len);
			rtu_framer_consume(framer, frame_len);
			framer->gap_deferred = false;
			framer->gap_deferred_len = 0;
			if (framer->pending.active)
				return false;
			continue;
		}

		if (keep_incomplete)
		{
			if (framer->rx_len <= MODBUS_RTU_MAX_FRAME)
			{
				/* Issue #49: in baud-derived auto mode, keep any incomplete
				 * buffer that is not already a valid frame until a second
				 * t3.5 gap passes. This covers short first chunks and
				 * variable/unknown-length requests (FC 08, MEI != 0x0E). */
				if (framer->gap_deferred)
				{
					if (framer->gap_deferred_len < framer->rx_len)
					{
						/* Kept bytes plus new data did not form a frame at offset 0.
						 * Retry from the first byte of new data so a valid request
						 * arriving after a truncated frame is not lost (issue #49). */
						u32 retry = framer->gap_deferred_len;
						framer->gap_deferred = false;
						framer->gap_deferred_len = 0;
						if (retry > 0 && retry < framer->rx_len)
						{
							rtu_framer_consume(framer, retry);
							if (framer->rx_len == 0)
								return false;
							continue;
						}
						framer->rx_len = 0;
						return false;
					}
					/* Second gap with no new data: discard incomplete bytes. */
					framer->rx_len = 0;
					framer->gap_deferred = false;
					framer->gap_deferred_len = 0;
					return false;
				}
				return true;
			}
			/* Oversized buffer cannot become a single valid RTU frame.
			 * Drop oldest bytes, but preserve any bytes received after the
			 * deferred boundary when possible. */
			if (framer->gap_deferred && framer->gap_deferred_len < framer->rx_len)
			{
				u32 retry = framer->gap_deferred_len;
				framer->gap_deferred = false;
				framer->gap_deferred_len = 0;
				rtu_framer_consume(framer, retry);
				if (framer->rx_len > MODBUS_RTU_MAX_FRAME)
					rtu_framer_consume(framer, framer->rx_len - MODBUS_RTU_MAX_FRAME);
				if (framer->rx_len == 0)
					return false;
				continue;
			}
			rtu_framer_consume(framer, framer->rx_len - MODBUS_RTU_MAX_FRAME);
			framer->gap_deferred = false;
			framer->gap_deferred_len = 0;
			if (framer->rx_len == 0)
				return false;
			continue;
		}

		framer->rx_len = 0;
		framer->gap_deferred = false;
		framer->gap_deferred_len = 0;
		return false;
	}

	if (framer->rx_len == 0)
	{
		framer->gap_deferred = false;
		framer->gap_deferred_len = 0;
	}
	return false;
}

void
rtu_framer_on_gap_due(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms, u64 now_us)
{
	bool incomplete = rtu_framer_process(framer, ctx, now_ms);

	framer->have_rx = framer->rx_len > 0;
	if (incomplete && !framer->gap_deferred)
	{
		framer->gap_deferred = true;
		framer->gap_deferred_len = framer->rx_len;
		framer->last_rx_ms = now_ms;
		framer->last_rx_us = now_us;
		return;
	}
	if (incomplete)
	{
		framer->rx_len = 0;
		framer->have_rx = false;
		framer->gap_deferred = false;
		framer->gap_deferred_len = 0;
		return;
	}
	framer->gap_deferred = false;
	framer->gap_deferred_len = 0;
}

static u32
rtu_char_gap_us(u32 baud, u32 fixed_us, u64 char_times_x10)
{
	u64	us;

	if (baud == 0)
		return 0;
	if (baud > RTU_BAUD_FIXED_THRESHOLD)
		return fixed_us;

	/* char time = 11 bits / baud; t1.5/t3.5 are char_times_x10 / 10. */
	us = (char_times_x10 * 11ULL * 1000000ULL + (u64)baud * 10ULL - 1ULL) / ((u64)baud * 10ULL);
	if (us > 0xFFFFFFFFULL)
		return 0xFFFFFFFFU;
	return (u32)us;
}

u32
rtu_t35_us(u32 baud)
{
	return rtu_char_gap_us(baud, RTU_INTERFRAME_GAP_US_HIGH, 35ULL);
}

u32
rtu_t15_us(u32 baud)
{
	return rtu_char_gap_us(baud, RTU_INTERCHAR_GAP_US_HIGH, 15ULL);
}

u32
rtu_effective_gap_ms(u32 frame_gap_ms, u32 baud)
{
	u32	t35;

	if (frame_gap_ms > 0)
		return frame_gap_ms;

	t35 = rtu_t35_us(baud);
	if (t35 > 0)
		return (t35 + 999U) / 1000U; /* ceil to whole ms for poll scheduling */

	return RTU_DEFAULT_FRAME_GAP_MS;
}

bool
rtu_gap_elapsed(u64 now_ms, u64 last_rx_ms, u32 gap_ms)
{
	/* A clock sample taken before last_rx_ms must not wrap into a huge
	 * elapsed time; it simply means no gap has been observed yet. */
	return now_ms >= last_rx_ms && now_ms - last_rx_ms >= gap_ms;
}

void
rtu_framer_note_gap(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms, u64 now_us)
{
	u32 t35;

	if (!framer->have_rx || framer->rx_len == 0 || framer->pending.active)
		return;
	if (ctx->frame_gap_ms > 0 || ctx->baud == 0)
		return;
	if (now_us < framer->last_rx_us)
		return;
	t35 = rtu_t35_us(ctx->baud);
	if (t35 == 0)
		return;
	{
		u64 elapsed = now_us - framer->last_rx_us;
		if (elapsed >= t35)
		{
			rtu_framer_on_gap_due(framer, ctx, now_ms, now_us);
			return;
		}
		if (ctx->strict_t15)
		{
			u32 t15 = rtu_t15_us(ctx->baud);
			if (t15 > 0 && elapsed >= t15)
			{
				framer->rx_len = 0;
				framer->have_rx = false;
				framer->gap_deferred = false;
				framer->gap_deferred_len = 0;
			}
		}
	}
}
