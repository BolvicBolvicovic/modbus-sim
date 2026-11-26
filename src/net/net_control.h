/**
 * @file
 * @brief Runtime control API behind the HTTP endpoint: `/api/...` reads and writes device
 *        tables and sets or clears faults while the server runs.
 *
 * Runs on the server thread inside net_poll(), so it needs no locking. JSON in and out, no
 * allocation. Opt-in with `(http (control on))`.
 */
#ifndef NET_CONTROL_H
#define NET_CONTROL_H

#include "net/net_obs.h"

/**
 * @brief Serves one `/api/` request.
 *
 * @param obs Server state: devices, transport rules for faults, request counters.
 * @param method HTTP method, e.g. "GET".
 * @param path Path without the query, starting with `/api/`.
 * @param query Text after `?` (without it), or "".
 * @param body NUL-terminated request body, or "".
 * @param out Receives the JSON response body (NUL-terminated).
 * @param cap Size of @p out.
 * @param status Receives the HTTP status code.
 * @return Length of the JSON written to @p out.
 */
u32	net_control_handle(net_obs_t* obs, const char* method, const char* path, const char* query,
			   const char* body, char* out, u32 cap, int* status);

#endif
