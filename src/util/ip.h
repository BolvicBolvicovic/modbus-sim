/**
 * @file
 * @brief IPv4 and IPv6 literal address validation.
 */
#ifndef IP_H
#define IP_H

#include <stdbool.h>

/** @brief Returns true when `text` is a valid IPv4 or IPv6 literal address. */
bool	ip_is_literal(const char* text);

#endif
