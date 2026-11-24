#ifndef NET_COMMON_H
#define NET_COMMON_H

#include "modbus/ascii.h"
#include "vlib/c_types.h"
#include <signal.h>
#include <stdbool.h>

typedef struct device_t device_t;
typedef struct rng_t rng_t;
typedef struct fault_decision_t fault_decision_t;

/* Shared delayed-response slot for byte-stream servers. ASCII frames are the
 * larger wire format (hex-encoded PDU plus LRC and CRLF), so size for ASCII;
 * RTU frames fit as well. */
typedef struct pending_response_t pending_response_t;
struct pending_response_t
{
	bool	active;
	u8	buf[MODBUS_ASCII_MAX_FRAME];
	u32	len;
	u32	sent;
	u64	due_ms;
};

/* Returns the device with the given unit id, or NULL. */
device_t*	net_find_device(device_t** devices, u32 count, u8 unit);

/* Installs SIGINT/SIGTERM handlers that set *stop_flag and ignores SIGPIPE.
 * The flag must remain valid for the lifetime of signal handling. */
void		net_install_stop_handlers(volatile sig_atomic_t* stop_flag);

/* Runs the PDU against the device and decides the response fault. */
u32		net_handle_pdu(device_t* dev, rng_t* rng, u64 now_ms,
			       const u8* req, u32 req_len, u8* resp,
			       fault_decision_t* decision);

/* Counts one bus communication error (FC 08 subfunction 0x000C) for every
 * device on the bus. Used for bad frames on both RTU and ASCII byte streams. */
void		net_record_comm_error(device_t** devices, u32 count);

/* Clears a queued response slot. */
void		pending_response_clear(pending_response_t* pending);

/* Drops a response that cannot be delivered after POLLHUP and stops the
 * byte-stream server loop. */
bool		net_handle_hangup(pending_response_t* pending);

/* Opens, binds, and listens on a nonblocking TCP socket. bind_address must be
 * an IPv4 or IPv6 literal. IPv6 sockets set IPV6_V6ONLY. Returns fd or -1. */
int		net_tcp_listen(const char* bind_address, u16 port, int backlog);

#endif
