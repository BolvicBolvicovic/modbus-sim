#ifndef RTU_FRAMER_H
#define RTU_FRAMER_H

#include "modbus/rtu.h"
#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct device_t device_t;
typedef struct rng_t rng_t;

typedef struct rtu_pending_t rtu_pending_t;
struct rtu_pending_t
{
	bool	active;
	u8	buf[MODBUS_RTU_MAX_FRAME];
	u32	len;
	u32	sent;
	u64	due_ms;
};

typedef struct rtu_framer_t rtu_framer_t;
struct rtu_framer_t
{
	u8		rx[MODBUS_RTU_MAX_FRAME * 4];
	u32		rx_len;
	u64		last_rx_ms;
	u64		last_rx_us;
	bool		have_rx;
	bool		gap_deferred;
	u32		gap_deferred_len;
	rtu_pending_t	pending;
};

void	rtu_pending_clear(rtu_pending_t* pending);
void	rtu_framer_init(rtu_framer_t* framer);
void	rtu_framer_consume(rtu_framer_t* framer, u32 n);
bool	rtu_framer_queue(rtu_framer_t* framer, u8 addr, const u8* pdu, u32 pdu_len, u64 due_ms);

/* Processes complete frames after a gap/EOF. Returns true when an incomplete
 * known-length frame was kept (baud-derived auto mode only). */
bool	rtu_framer_process(rtu_framer_t* framer, device_t** devices, u32 device_count,
			   rng_t* rng, u64 now_ms, u32 frame_gap_ms, u32 baud,
			   const char* peer);

/* Applies RTU timing on byte streams. strict_t15 is best-effort and should be
 * false for RTU-over-TCP. */
void	rtu_framer_note_gap(rtu_framer_t* framer, device_t** devices, u32 device_count,
			    rng_t* rng, u64 now_ms, u64 now_us, u32 frame_gap_ms,
			    u32 baud, bool strict_t15, const char* peer);

#endif
