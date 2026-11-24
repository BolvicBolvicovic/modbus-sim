#include "util/ip.h"
#include <arpa/inet.h>
#include <netinet/in.h>

bool
ip_is_literal(const char* text)
{
	struct in_addr	addr4;
	struct in6_addr	addr6;

	if (!text)
		return false;

	return inet_pton(AF_INET, text, &addr4) == 1 ||
	       inet_pton(AF_INET6, text, &addr6) == 1;
}
