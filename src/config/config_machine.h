#ifndef CONFIG_MACHINE_H
#define CONFIG_MACHINE_H

#include "config/form.h"
#include "device/machine.h"

/* Compiles one (machine ...) device item into a machine_def_t and stores a
 * deep copy on the device. Reports load errors through ctx->err. */
bool	config_machine_add(form_ctx_t* ctx, s7_pointer form, device_t* dev);

/* Validates deferred point references while their form text is still in the
 * scratch state. Called once, after the last item of a device. */
bool	config_machine_check_points(form_ctx_t* ctx, device_t* dev);

/* Initializes every machine on dev at dev->start_ms using the seed already set
 * by the caller via device_seed_machines(). Called after the whole config is
 * read, so the server seed is known. */
bool	config_machine_finalize(form_ctx_t* ctx, device_t* dev);

#endif
