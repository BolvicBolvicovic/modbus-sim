#define _GNU_SOURCE
#include "net/net_common.h"
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
net_tcp_listen(const char* bind_address, u16 port, int backlog)
{
	struct sockaddr_storage	addr = { 0 };
	socklen_t		addr_len = 0;
	struct in_addr		addr4;
	struct in6_addr		addr6;
	int			family = AF_UNSPEC;
	int			yes = 1;
	int			fd;

	if (inet_pton(AF_INET, bind_address, &addr4) == 1)
	{
		struct sockaddr_in* a = (struct sockaddr_in*)&addr;

		family = AF_INET;
		a->sin_family = AF_INET;
		a->sin_port = htons(port);
		a->sin_addr = addr4;
		addr_len = sizeof(*a);
	}
	else if (inet_pton(AF_INET6, bind_address, &addr6) == 1)
	{
		struct sockaddr_in6* a = (struct sockaddr_in6*)&addr;

		family = AF_INET6;
		a->sin6_family = AF_INET6;
		a->sin6_port = htons(port);
		a->sin6_addr = addr6;
		addr_len = sizeof(*a);
	}
	else
	{
		LOG_WARN("bind address '%s' is not a valid IPv4 or IPv6 address", bind_address);
		return -1;
	}

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
