#ifndef IP_H
#define IP_H

#include <stdbool.h>

/* Returns true when `text` is a valid IPv4 literal (dotted quad). */
bool	ip_is_ipv4_literal(const char* text);

#endif
