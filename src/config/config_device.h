#ifndef CONFIG_DEVICE_H
#define CONFIG_DEVICE_H

#include "config/form.h"
#include "device/device.h"

/* Name: config_parse_device
 * Description: turns one (device ...) form into a device allocated in `arena`.
 * Machines are compiled and their point references checked, but seeded and
 * finalized only after the whole config is read, when the server seed is known.
 * On failure returns false and ctx->err says what is wrong.
 * */
bool	config_parse_device(form_ctx_t* ctx, s7_pointer form, arena_t* arena,
			    u64 start_ms, device_t** out);

#endif
