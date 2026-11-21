#include "util/ip.h"
#include <arpa/inet.h>

bool
ip_is_ipv4_literal(const char* text)
{
	struct in_addr	addr;

	if (!text)
		return false;

	return inet_pton(AF_INET, text, &addr) == 1;
}
