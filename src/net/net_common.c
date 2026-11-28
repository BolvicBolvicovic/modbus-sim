/**
 * @file
 * @brief Helpers shared by the transports: device lookup, PDU and fault handling, MBAP replies, listeners.
 */
#define _GNU_SOURCE
#include "net/net_common.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "net/net_obs.h"
#include "util/log.h"
#include <stddef.h>
#include <string.h>

device_t*
net_find_device(device_t** devices, u32 count, u8 unit, bool serial)
{
	device_t* any = NULL;

	for (u32 i = 0; i < count; i++)
	{
		if (devices[i]->unit_id == unit)
			return devices[i];
		if (devices[i]->any_unit)
			any = devices[i];
	}
	/* RTU/ASCII reserve 248-255 and the any-unit device never answers them. */
	return serial && unit > 247 ? NULL : any;
}

u32
net_handle_pdu(net_obs_t* obs, const char* peer, device_t* dev, u8 unit, rng_t* rng, u64 now_ms,
	       const u8* req, u32 req_len, u8* resp, fault_decision_t* decision)
{
	u32 len;

	dev->request_unit = unit;
	len = modbus_handle_request(dev, rng, now_ms, req, req_len, resp, decision);

	net_obs_request(obs, peer, dev, unit, req[0], resp, len, decision);
	return len;
}

void
net_record_comm_error(net_obs_t* obs, device_t** devices, u32 count)
{
	net_obs_rejected(obs);
	for (u32 i = 0; i < count; i++)
	{
		device_t* dev = devices[i];

		dev->diag_comm_error_count = (u16)(dev->diag_comm_error_count + 1);
	}
}

void
pending_response_clear(pending_response_t* pending)
{
	pending->active = false;
	pending->len    = 0;
	pending->sent   = 0;
	pending->due_ms = 0;
}

bool
net_handle_hangup(pending_response_t* pending)
{
	pending_response_clear(pending);
	return false;
}

u32
net_bind_addrs(const char* bind_address, u16 port, bool datagram, os_addr_t* out)
{
	os_addr_t	found[NET_BIND_ADDR_MAX];
	u32		n;
	u32		count = 0;

	if (os_addr_parse(&out[0], bind_address, port))
		return 1;
	n = os_resolve(bind_address, port, datagram, found, NET_BIND_ADDR_MAX);
	if (n == 0)
	{
		LOG_WARN("bind address '%s' is not an IP address and does not resolve to one", bind_address);
		return 0;
	}
	/* IPv4 first, so "localhost" binds 127.0.0.1 whatever order the resolver answers in. */
	for (u32 i = 0; i < n; i++)
	{
		if (!os_addr_is_ipv6(&found[i]))
			out[count++] = found[i];
	}
	for (u32 i = 0; i < n; i++)
	{
		if (os_addr_is_ipv6(&found[i]))
			out[count++] = found[i];
	}
	return count;
}

os_socket_t
net_tcp_listen(const char* bind_address, u16 port, int backlog)
{
	os_addr_t	addrs[NET_BIND_ADDR_MAX];
	u32		n = net_bind_addrs(bind_address, port, false, addrs);

	for (u32 i = 0; i < n; i++)
	{
		os_socket_t s = os_tcp_listen(&addrs[i], backlog);

		if (s != OS_SOCKET_INVALID)
			return s;
		if (i + 1 == n)
			LOG_WARN("cannot listen on %s:%u: %s", bind_address, port, os_last_error_text());
	}
	return OS_SOCKET_INVALID;
}

u32
net_mbap_respond(net_obs_t* obs, device_t** devices, u32 count, rng_t* rng, u64 now_ms, const char* peer,
		 const mbap_header_t* header, const u8* request, u8* out,
		 fault_decision_t* decision)
{
	u8		pdu[MODBUS_MAX_PDU];
	device_t*	dev     = net_find_device(devices, count, header->unit, false);
	u32		req_len = (u32)header->length - 1;
	u32		pdu_len = 0;
	mbap_header_t	reply   = *header;

	if (!dev)
	{
		pdu_len = modbus_exception(pdu, request[0], EXC_GATEWAY_TARGET_FAILED);
		net_obs_request(obs, peer, NULL, header->unit, request[0], pdu, pdu_len, NULL);
	}
	else
		pdu_len = net_handle_pdu(obs, peer, dev, header->unit, rng, now_ms, request, req_len, pdu, decision);

	if (decision->drop || pdu_len == 0)
		return 0;
	reply.length = (u16)(pdu_len + 1);
	mbap_encode(out, &reply);
	memcpy(out + MBAP_HEADER_SIZE, pdu, pdu_len);
	return net_frame_trace(obs, NET_FRAME_SENT, peer, out, MBAP_HEADER_SIZE + pdu_len, MBAP_MAX_FRAME);
}
