#ifndef IP_H
#define IP_H

#include <stdbool.h>

/* Returns true when `text` is a valid IPv4 or IPv6 literal address. */
bool	ip_is_literal(const char* text);

#endif
