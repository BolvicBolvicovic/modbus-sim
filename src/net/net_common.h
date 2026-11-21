#ifndef NET_COMMON_H
#define NET_COMMON_H

#include "device/device.h"
#include "device/fault.h"
#include "util/rng.h"
#include <signal.h>

/* Returns the device with the given unit id, or NULL. */
device_t*	net_find_device(device_t** devices, u32 count, u8 unit);

/* Installs SIGINT/SIGTERM handlers that set *stop_flag and ignores SIGPIPE.
 * The flag must remain valid for the lifetime of signal handling. */
void		net_install_stop_handlers(volatile sig_atomic_t* stop_flag);

/* Runs the PDU against the device and decides the response fault. */
u32		net_handle_pdu(device_t* dev, rng_t* rng, u64 now_ms,
			       const u8* req, u32 req_len, u8* resp,
			       fault_decision_t* decision);

#endif
