#define _GNU_SOURCE
#include "net/net_common.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "util/log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

device_t*
net_find_device(device_t** devices, u32 count, u8 unit)
{
	for (u32 i = 0; i < count; i++)
	{
		if (devices[i]->unit_id == unit)
			return devices[i];
	}
	return NULL;
}

u32
net_handle_pdu(device_t* dev, rng_t* rng, u64 now_ms, const u8* req, u32 req_len,
	       u8* resp, fault_decision_t* decision)
{
	return modbus_handle_request(dev, rng, now_ms, req, req_len, resp, decision);
}

void
net_record_comm_error(device_t** devices, u32 count)
{
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

int
net_bind_addr(const char* bind_address, u16 port, struct sockaddr_storage* addr, socklen_t* addr_len)
{
	struct in_addr	addr4;
	struct in6_addr	addr6;

	memset(addr, 0, sizeof(*addr));
	if (inet_pton(AF_INET, bind_address, &addr4) == 1)
	{
		struct sockaddr_in* a = (struct sockaddr_in*)addr;

		a->sin_family = AF_INET;
		a->sin_port = htons(port);
		a->sin_addr = addr4;
		*addr_len = sizeof(*a);
		return AF_INET;
	}
	if (inet_pton(AF_INET6, bind_address, &addr6) == 1)
	{
		struct sockaddr_in6* a = (struct sockaddr_in6*)addr;

		a->sin6_family = AF_INET6;
		a->sin6_port = htons(port);
		a->sin6_addr = addr6;
		*addr_len = sizeof(*a);
		return AF_INET6;
	}
	LOG_WARN("bind address '%s' is not a valid IPv4 or IPv6 address", bind_address);
	return AF_UNSPEC;
}

int
net_tcp_listen(const char* bind_address, u16 port, int backlog)
{
	struct sockaddr_storage	addr = { 0 };
	socklen_t		addr_len = 0;
	int			family = net_bind_addr(bind_address, port, &addr, &addr_len);
	int			yes = 1;
	int			fd;

	if (family == AF_UNSPEC)
		return -1;

	fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		LOG_WARN("cannot create socket for %s:%u: %s", bind_address, port, strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));

	if (bind(fd, (struct sockaddr*)&addr, addr_len) < 0 || listen(fd, backlog) < 0)
	{
		LOG_WARN("cannot listen on %s:%u: %s", bind_address, port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

u32
net_mbap_respond(device_t** devices, u32 count, rng_t* rng, u64 now_ms, const char* peer,
		 const mbap_header_t* header, const u8* request, u8* out,
		 fault_decision_t* decision)
{
	u8		pdu[MODBUS_MAX_PDU];
	device_t*	dev     = net_find_device(devices, count, header->unit);
	u32		req_len = (u32)header->length - 1;
	u32		pdu_len = 0;
	mbap_header_t	reply   = *header;

	if (!dev)
		pdu_len = modbus_exception(pdu, request[0], EXC_GATEWAY_TARGET_FAILED);
	else
		pdu_len = net_handle_pdu(dev, rng, now_ms, request, req_len, pdu, decision);

	LOG_DEBUG("%s tid=%u unit=%u fc=0x%02X -> %s%s", peer, header->transaction,
		  header->unit, request[0], pdu_len == 0 ? "no response" :
		  (pdu[0] & 0x80) ? "exception" : "ok",
		  decision->drop ? " (dropped by fault)" : decision->delay_ms ? " (delayed by fault)" : "");

	if (decision->drop || pdu_len == 0)
		return 0;
	reply.length = (u16)(pdu_len + 1);
	mbap_encode(out, &reply);
	memcpy(out + MBAP_HEADER_SIZE, pdu, pdu_len);
	return MBAP_HEADER_SIZE + pdu_len;
}
