#ifndef NET_COMMON_H
#define NET_COMMON_H

#include "modbus/ascii.h"
#include "modbus/mbap.h"
#include "vlib/c_types.h"
#include <stdbool.h>
#include "vlib/macros.h"
#include <sys/socket.h>

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

/* Result of every X_server_step(). The net layer never installs signal
 * handlers; a host stops a server with X_server_stop() and keeps stepping
 * until a non-zero result. */
enum
{
	NET_STEP_CONTINUE = 0,
	NET_STEP_DONE     = 1,	/* stopped, or the byte-stream peer went away */
	NET_STEP_ERROR    = 2,	/* poll failed */
};

/* Returns the device with the given unit id, or NULL. */
device_t*	net_find_device(device_t** devices, u32 count, u8 unit);

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

/* Parses an IPv4 or IPv6 literal plus port into a socket address. Returns the
 * address family, or AF_UNSPEC (after a WARN) when bind_address is not a literal. */
int		net_bind_addr(const char* bind_address, u16 port,
			      struct sockaddr_storage* addr, socklen_t* addr_len);

/* Handles one validated MBAP request (`request` = the PDU after the header) and
 * writes the complete reply frame to `out` (MBAP_MAX_FRAME bytes): unknown units
 * get exception 0x0B like every MBAP transport. Returns the frame length, or 0
 * when nothing is to be sent (no response or drop fault). `peer` is for logs. */
u32		net_mbap_respond(device_t** devices, u32 count, rng_t* rng, u64 now_ms,
				 const char* peer, const mbap_header_t* header,
				 const u8* request, u8* out, fault_decision_t* decision);

/* Opens, binds, and listens on a nonblocking TCP socket. bind_address must be
 * an IPv4 or IPv6 literal. IPv6 sockets set IPV6_V6ONLY. Returns fd or -1. */
int		net_tcp_listen(const char* bind_address, u16 port, int backlog);

#endif
