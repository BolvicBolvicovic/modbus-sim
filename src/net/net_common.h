#ifndef NET_COMMON_H
#define NET_COMMON_H

#include "vlib/c_types.h"
#include <signal.h>

typedef struct device_t device_t;
typedef struct rng_t rng_t;
typedef struct fault_decision_t fault_decision_t;

/* Returns the device with the given unit id, or NULL. */
device_t*	net_find_device(device_t** devices, u32 count, u8 unit);

/* Installs SIGINT/SIGTERM handlers that set *stop_flag and ignores SIGPIPE.
 * The flag must remain valid for the lifetime of signal handling. */
void		net_install_stop_handlers(volatile sig_atomic_t* stop_flag);

/* Runs the PDU against the device and decides the response fault. */
u32		net_handle_pdu(device_t* dev, rng_t* rng, u64 now_ms,
			       const u8* req, u32 req_len, u8* resp,
			       fault_decision_t* decision);

/* Counts one RTU bus communication error for every device on the bus. */
void		net_record_rtu_comm_error(device_t** devices, u32 count);

/* Opens, binds, and listens on a nonblocking IPv4 TCP socket. Returns fd or -1. */
int		net_tcp_listen(const char* bind_address, u16 port, int backlog);

#endif
