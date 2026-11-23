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

static volatile sig_atomic_t* volatile	g_stop_flag = NULL;

static void
on_stop_signal(int signo)
{
	(void)signo;
	if (g_stop_flag)
		*g_stop_flag = 1;
}

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

void
net_install_stop_handlers(volatile sig_atomic_t* stop_flag)
{
	struct sigaction	sa = { 0 };

	g_stop_flag = stop_flag;
	sa.sa_handler = on_stop_signal;	/* no SA_RESTART: poll() must wake up */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

u32
net_handle_pdu(device_t* dev, rng_t* rng, u64 now_ms, const u8* req, u32 req_len,
	       u8* resp, fault_decision_t* decision)
{
	return modbus_handle_request(dev, rng, now_ms, req, req_len, resp, decision);
}

int
net_tcp_listen(const char* bind_address, u16 port, int backlog)
{
	struct sockaddr_in	addr = { 0 };
	int			yes  = 1;
	int			fd   = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(port);
	if (inet_pton(AF_INET, bind_address, &addr.sin_addr) != 1)
	{
		LOG_WARN("bind address '%s' is not an IPv4 address", bind_address);
		close(fd);
		return -1;
	}
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, backlog) < 0)
	{
		LOG_WARN("cannot listen on %s:%u: %s", bind_address, port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}
