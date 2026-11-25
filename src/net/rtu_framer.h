#ifndef RTU_FRAMER_H
#define RTU_FRAMER_H

#include "modbus/rtu.h"
#include "net/net_common.h"
#include "vlib/c_types.h"
#include <stdbool.h>

#define RTU_DEFAULT_FRAME_GAP_MS	20
#define RTU_BAUD_FIXED_THRESHOLD	19200U
#define RTU_INTERCHAR_GAP_US_HIGH	750U	/* t1.5 at >19200 baud */
#define RTU_INTERFRAME_GAP_US_HIGH	1750U	/* t3.5 at >19200 baud */

typedef struct device_t device_t;
typedef struct rng_t rng_t;

typedef struct rtu_framer_t rtu_framer_t;
struct rtu_framer_t
{
	u8		rx[MODBUS_RTU_MAX_FRAME * 4];
	u32		rx_len;
	u64		last_rx_ms;
	u64		last_rx_us;
	bool		have_rx;
	bool		fault_disconnect;	/* handled frame asked the TCP transport to close */
	bool		gap_deferred;
	u32		gap_deferred_len;
	pending_response_t	pending;
};

/* Name: rtu_framer_ctx_t
 * Description: shared RTU frame processing context. Borrowed pointers; valid for the
 * duration of the server run. frame_gap_ms == 0 means baud-derived/auto timing.
 * strict_t15 is best-effort and only meaningful for RTU byte streams.
 * broadcast_execute runs unit-0 requests on all devices without a response. */
typedef struct rtu_framer_ctx_t rtu_framer_ctx_t;
struct rtu_framer_ctx_t
{
	device_t**	devices;
	u32		device_count;
	rng_t*		rng;
	u32		frame_gap_ms;
	u32		baud;
	bool		strict_t15;
	bool		broadcast_execute;
	const char*	peer;
};

void	rtu_framer_init(rtu_framer_t* framer);
void	rtu_framer_consume(rtu_framer_t* framer, u32 n);
bool	rtu_framer_queue(rtu_framer_t* framer, u8 addr, const u8* pdu, u32 pdu_len,
			 u64 due_ms, bool bad_checksum);

/* Processes complete frames after a gap/EOF. Returns true when an incomplete
 * frame was kept for one extra gap window (baud-derived auto mode only). */
bool	rtu_framer_process(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms);

/* Applies RTU timing on byte streams. strict_t15 is best-effort and should be
 * false for RTU-over-TCP. */
void	rtu_framer_note_gap(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms, u64 now_us);

/* True when a non-empty frame is waiting for its inter-frame gap (or for the
 * read side to close). read_closed is owned by the transport, not the framer. */
bool	rtu_framer_gap_due(const rtu_framer_t* framer, bool read_closed, u64 now_ms, u32 gap_ms);

/* Handles a due inter-frame gap: process, then defer once or discard incomplete
 * bytes. Updates framer timing/deferred state. */
void	rtu_framer_on_gap_due(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms, u64 now_us);

/* Retries a deferred incomplete frame after new bytes arrive (baud-derived auto
 * mode only). No-op unless the new bytes can advance the deferral. */
void	rtu_framer_on_new_data(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms);

/* Name: rtu_t35_us
 * Description: Modbus RTU t3.5 inter-frame gap in microseconds for `baud`.
 * Returns 0 when baud is unknown/0. For baud > 19200 Modbus specifies fixed
 * 1750 us; otherwise t3.5 = ceil(3.5 * 11 * 1_000_000 / baud).
 * */
u32	rtu_t35_us(u32 baud);

/* Name: rtu_t15_us
 * Description: Modbus RTU t1.5 inter-character gap in microseconds for `baud`.
 * Returns 0 when baud is unknown/0. For baud > 19200 Modbus specifies fixed
 * 750 us; otherwise t1.5 = ceil(1.5 * 11 * 1_000_000 / baud).
 * */
u32	rtu_t15_us(u32 baud);

/* Name: rtu_effective_gap_ms
 * Description: resolves the inter-frame gap used by the RTU server.
 * frame_gap_ms > 0 wins (explicit override). Otherwise baud-derived t3.5 is
 * used (rounded up to whole ms for poll/clock-ms scheduling). If both are
 * unknown, returns RTU_DEFAULT_FRAME_GAP_MS.
 * */
u32	rtu_effective_gap_ms(u32 frame_gap_ms, u32 baud);

/* Name: rtu_gap_elapsed
 * Description: true when at least `gap_ms` elapsed between `last_rx_ms` and
 * `now_ms`. Safe when the clock reading is older than the last receive (for
 * example because they were sampled at different points), which must not look
 * like an elapsed gap.
 * */
bool	rtu_gap_elapsed(u64 now_ms, u64 last_rx_ms, u32 gap_ms);

#endif
