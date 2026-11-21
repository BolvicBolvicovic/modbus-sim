#include "net/net_common.h"
#include "modbus/pdu.h"
#include <stddef.h>

static volatile sig_atomic_t*	g_stop_flag = NULL;

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
	u32	len = modbus_handle_pdu(dev, now_ms, req, req_len, resp);

	*decision = fault_decide(&dev->fault, rng);
	return len;
}
