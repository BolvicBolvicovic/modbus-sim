/**
 * @file
 * @brief RTU receive framing shared by the RTU byte-stream, TCP and UDP servers.
 */
#ifndef RTU_FRAMER_H
#define RTU_FRAMER_H

#include "modbus/rtu.h"
#include "net/net_common.h"
#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct device_t device_t;
typedef struct rng_t rng_t;
typedef struct rtu_framer_t rtu_framer_t;
typedef struct net_obs_t net_obs_t;
typedef struct rtu_framer_ctx_t rtu_framer_ctx_t;

#define RTU_DEFAULT_FRAME_GAP_MS	20
#define RTU_BAUD_FIXED_THRESHOLD	19200U
#define RTU_INTERCHAR_GAP_US_HIGH	750U	/**< t1.5 at >19200 baud */
#define RTU_INTERFRAME_GAP_US_HIGH	1750U	/**< t3.5 at >19200 baud */

/** @brief Receive buffer and timing state that split a byte stream into RTU frames. */
struct rtu_framer_t
{
	u8		rx[MODBUS_RTU_MAX_FRAME * 4];
	u32		rx_len;
	u64		last_rx_ms;
	u64		last_rx_us;
	bool		have_rx;
	bool		fault_disconnect;	/**< handled frame asked the TCP transport to close */
	bool		gap_deferred;
	u32		gap_deferred_len;
	u64		last_char_us;	/**< char timing: estimated arrival of the newest character; 0 = none yet */
	bool		skipping;	/**< char timing: t1.5 was exceeded, drop characters until t3.5 of silence */
	pending_response_t	pending;
};

/**
 * @brief Shared RTU frame processing context.
 *
 * Borrowed pointers; valid for the duration of the server run. frame_gap_ms == 0 means
 * baud-derived/auto timing. strict_t15 is best-effort and only meaningful for RTU byte
 * streams. broadcast_execute runs unit-0 requests on all devices without a response.
 * char_timing (needs a baud) measures t1.5 and t3.5 between estimated character arrival times
 * (rtu_framer_rx_chars) and replaces the strict_t15 heuristic and the deferral of incomplete
 * frames.
 */
struct rtu_framer_ctx_t
{
	device_t**	devices;
	u32		device_count;
	rng_t*		rng;
	u32		frame_gap_ms;
	u32		baud;
	bool		strict_t15;
	bool		char_timing;
	bool		broadcast_execute;
	const char*	peer;		/**< client for logs, NULL on a byte stream */
	net_obs_t*	obs;		/**< metrics and request logs; NULL records nothing */
};

void	rtu_framer_init(rtu_framer_t* framer);
void	rtu_framer_consume(rtu_framer_t* framer, u32 n);
bool	rtu_framer_queue(rtu_framer_t* framer, u8 addr, const u8* pdu, u32 pdu_len,
			 u64 due_ms, bool bad_checksum);

/**
 * @brief Executes one complete frame (CRC-checked here): broadcast handling, unit lookup,
 *        fault decision, and the reply queued into framer->pending.
 *
 * Used by datagram transports, where the datagram is the frame. The caller has already shown the
 * frame to the frame hook; the reply is shown to it here, and a drop leaves nothing queued.
 */
void	rtu_framer_handle_frame(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms,
				const u8* frame, u32 len);

/**
 * @brief Processes complete frames after a gap/EOF.
 *
 * Returns true when an incomplete frame was kept for one extra gap window (baud-derived auto
 * mode only).
 */
bool	rtu_framer_process(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms);

/**
 * @brief Applies RTU timing on byte streams. strict_t15 is best-effort and should be false for
 *        RTU-over-TCP.
 */
void	rtu_framer_note_gap(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms, u64 now_us);

/**
 * @brief Character-level receive (ctx->char_timing): feeds `n` characters that one read() returned
 *        at `now_us` and applies t1.5 and t3.5 between them.
 *
 * User space cannot see when each character really arrived, so the newest one is dated `now_us`
 * and each earlier one a character time earlier, never closer than one character time to its
 * predecessor (the line carries one character at a time). A silence over t1.5 inside a frame
 * discards it and every character up to the next silence of t3.5; a silence of t3.5 ends the
 * frame (processed, an incomplete one discarded). The frame still ends at the poll timeout when
 * no further character follows.
 */
void	rtu_framer_rx_chars(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, const u8* data, u32 n,
			    u64 now_ms, u64 now_us);

/**
 * @brief True when a non-empty frame is waiting for its inter-frame gap (or for the read side
 *        to close). read_closed is owned by the transport, not the framer.
 */
bool	rtu_framer_gap_due(const rtu_framer_t* framer, bool read_closed, u64 now_ms, u32 gap_ms);

/**
 * @brief Handles a due inter-frame gap: process, then defer once or discard incomplete bytes.
 *
 * Updates framer timing/deferred state.
 */
void	rtu_framer_on_gap_due(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms, u64 now_us);

/**
 * @brief Retries a deferred incomplete frame after new bytes arrive (baud-derived auto mode
 *        only).
 *
 * No-op unless the new bytes can advance the deferral.
 */
void	rtu_framer_on_new_data(rtu_framer_t* framer, const rtu_framer_ctx_t* ctx, u64 now_ms);

/**
 * @brief Modbus RTU t3.5 inter-frame gap in microseconds for `baud`.
 *
 * Returns 0 when baud is unknown/0. For baud > 19200 Modbus specifies fixed 1750 us; otherwise
 * t3.5 = ceil(3.5 * 11 * 1_000_000 / baud).
 */
u32	rtu_t35_us(u32 baud);

/**
 * @brief Modbus RTU t1.5 inter-character gap in microseconds for `baud`.
 *
 * Returns 0 when baud is unknown/0. For baud > 19200 Modbus specifies fixed 750 us; otherwise
 * t1.5 = ceil(1.5 * 11 * 1_000_000 / baud).
 */
u32	rtu_t15_us(u32 baud);

/** @brief Time of one RTU character (11 bits) in microseconds for `baud`; 0 when baud is 0. */
u32	rtu_char_us(u32 baud);

/**
 * @brief Resolves the inter-frame gap used by the RTU server. frame_gap_ms > 0 wins (explicit
 *        override).
 *
 * Otherwise baud-derived t3.5 is used (rounded up to whole ms for poll/clock-ms scheduling).
 * If both are unknown, returns RTU_DEFAULT_FRAME_GAP_MS.
 */
u32	rtu_effective_gap_ms(u32 frame_gap_ms, u32 baud);

/**
 * @brief True when at least `gap_ms` elapsed between `last_rx_ms` and `now_ms`.
 *
 * Safe when the clock reading is older than the last receive (for example because they were
 * sampled at different points), which must not look like an elapsed gap.
 */
bool	rtu_gap_elapsed(u64 now_ms, u64 last_rx_ms, u32 gap_ms);

#endif
