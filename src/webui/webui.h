/**
 * @file
 * @brief The built-in browser UI: a static page embedded in the binary.
 *
 * The page source is `src/webui/index.html`; the Makefile turns it into a byte array at build
 * time. The module knows nothing of the rest of mogbus: the HTTP endpoint in net/ asks it for a
 * path and sends the bytes.
 */
#ifndef WEBUI_H
#define WEBUI_H

#include "vlib/c_types.h"
#include <stdbool.h>

/**
 * @brief Looks up the static content served at `path`.
 *
 * @param path Request path without the query string, not NUL terminated.
 * @param path_len Length of @p path.
 * @param body Receives the bytes (static, never freed).
 * @param len Receives the number of bytes.
 * @param content_type Receives the Content-Type value (static).
 * @return true for `/` and `/index.html`, false for any other path.
 */
bool	webui_serve(const char* path, u32 path_len, const u8** body, u32* len, const char** content_type);

#endif
