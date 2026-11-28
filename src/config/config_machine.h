/**
 * @file
 * @brief Compiler for (machine ...) forms.
 */
#ifndef CONFIG_MACHINE_H
#define CONFIG_MACHINE_H

#include "config/form.h"
#include "device/action.h"
#include "device/machine.h"

/**
 * @brief Compiles one (machine ...) device item into a machine_def_t and stores a deep copy on
 *        the device.
 *
 * Reports load errors through ctx->err.
 */
bool	config_machine_add(form_ctx_t* ctx, s7_pointer form, device_t* dev);

/**
 * @brief Validates deferred point references while their form text is still in the scratch
 *        state.
 *
 * Called once, after the last item of a device.
 */
bool	config_machine_check_points(form_ctx_t* ctx, device_t* dev);

/**
 * @brief Initializes every machine on dev at dev->start_ms using the seed already set by the
 *        caller via device_seed_machines().
 *
 * Called after the whole config is read, so the server seed is known.
 */
bool	config_machine_finalize(form_ctx_t* ctx, device_t* dev);

/**
 * @brief Records a top-level (define-action ...) form for later (on-read ...)/(on-write ...)
 *        uses.
 *
 * Checks its name, params, vars and the shape of its statements; the statements are compiled
 * at each use, where `self` and the points are known.
 */
bool	config_action_define(form_ctx_t* ctx, s7_pointer form);

/**
 * @brief Queues the body of an on-read (`event` ACTION_ON_READ) or on-write action of the point
 *        at `space`/`address`; it is compiled by config_action_compile().
 */
bool	config_action_defer(form_ctx_t* ctx, space_kind_t space, u32 address,
			    action_event_t event, s7_pointer body);

/**
 * @brief Compiles the queued actions of `dev` and stores them on it.
 *
 * Called after the last item of a device, when every point it can name exists.
 */
bool	config_action_compile(form_ctx_t* ctx, device_t* dev);

/** @brief Checks the action targets once machines own their points (after config_machine_finalize()). */
bool	config_action_finalize(form_ctx_t* ctx, device_t* dev);

#endif
