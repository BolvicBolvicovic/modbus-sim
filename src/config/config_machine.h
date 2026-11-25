#ifndef CONFIG_MACHINE_H
#define CONFIG_MACHINE_H

#include "config/form.h"
#include "device/machine.h"

/* Compiles one (machine ...) device item into a machine_def_t and stores a
 * deep copy on the device. Reports load errors through ctx->err. */
bool	config_machine_add(form_ctx_t* ctx, s7_pointer form, device_t* dev);

/* Validates deferred point references and finalizes every machine on dev.
 * Called once, after the last device item. */
bool	config_machine_finalize(form_ctx_t* ctx, device_t* dev);

#endif
