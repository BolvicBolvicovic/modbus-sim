/**
 * @file
 * @brief Helpers and types shared by all transport servers.
 */
#ifndef NET_COMMON_H
#define NET_COMMON_H

#include "modbus/ascii.h"
#include "modbus/mbap.h"
#include "vlib/c_types.h"
#include <stdbool.h>
#include "vlib/macros.h"
#include "os/os_socket.h"

typedef struct device_t device_t;
typedef struct rng_t rng_t;
typedef struct fault_decision_t fault_decision_t;
typedef struct net_obs_t net_obs_t;
typedef struct pending_response_t pending_response_t;

/**
 * @brief Shared delayed-response slot for byte-stream servers.
 *
 * ASCII frames are the larger wire format (hex-encoded PDU plus LRC and CRLF), so size for
 * ASCII; RTU frames fit as well.
 */
struct pending_response_t
{
	bool	active;
	u8	buf[MODBUS_ASCII_MAX_FRAME];
	u32	len;
	u32	sent;
	u64	due_ms;
};

/**
 * @brief Result of every X_server_step().
 *
 * The net layer never installs signal handlers; a host stops a server with X_server_stop() and
 * keeps stepping until a non-zero result.
 */
enum
{
	NET_STEP_CONTINUE = 0,
	NET_STEP_DONE     = 1,	/**< stopped, or the byte-stream peer went away */
	NET_STEP_ERROR    = 2,	/**< wait failed */
};

/**
 * @brief Returns the device with the given unit id, else the any-unit device, else NULL.
 *
 * With `serial` (RTU/ASCII) units 248-255 are reserved and never fall back to the any-unit
 * device.
 */
device_t*	net_find_device(device_t** devices, u32 count, u8 unit, bool serial);

/**
 * @brief Runs the PDU against the device, decides the response fault, and records and logs the
 *        request in `obs` (NULL records nothing).
 *
 * `peer` describes the client in logs (NULL for a byte stream); `unit` is the request's unit
 * id, which differs from the device's own for an any-unit device.
 */
u32		net_handle_pdu(net_obs_t* obs, const char* peer, device_t* dev, u8 unit, rng_t* rng, u64 now_ms,
			       const u8* req, u32 req_len, u8* resp, fault_decision_t* decision);

/**
 * @brief Counts one bus communication error (FC 08 subfunction 0x000C) for every device on the
 *        bus.
 *
 * Used for bad frames on both RTU and ASCII byte streams.
 */
void		net_record_comm_error(net_obs_t* obs, device_t** devices, u32 count);

/** @brief Clears a queued response slot. */
void		pending_response_clear(pending_response_t* pending);

/**
 * @brief Drops a response that cannot be delivered after a hangup and stops the byte-stream
 *        server loop.
 */
bool		net_handle_hangup(pending_response_t* pending);

/**
 * @brief Parses an IPv4 or IPv6 literal plus port into a socket address.
 *
 * Returns false (after a WARN) when bind_address is not a literal.
 */
bool		net_bind_addr(const char* bind_address, u16 port, os_addr_t* addr);

/**
 * @brief Handles one validated MBAP request (`request` = the PDU after the header) and writes
 *        the complete reply frame to `out` (MBAP_MAX_FRAME bytes): unknown units get exception 0x0B
 *        like every MBAP transport.
 *
 * Returns the frame length, or 0 when nothing is to be sent (no response or drop fault).
 * `peer` is for logs.
 */
u32		net_mbap_respond(net_obs_t* obs, device_t** devices, u32 count, rng_t* rng, u64 now_ms,
				 const char* peer, const mbap_header_t* header,
				 const u8* request, u8* out, fault_decision_t* decision);

/**
 * @brief Opens, binds, and listens on a nonblocking TCP socket. bind_address must be an IPv4
 *        or IPv6 literal.
 *
 * IPv6 sockets set IPV6_V6ONLY. Returns the socket or OS_SOCKET_INVALID.
 */
os_socket_t	net_tcp_listen(const char* bind_address, u16 port, int backlog);

#endif
