/**
 * @file
 * @brief Shared setup for the libFuzzer harnesses: one device that owns something in every
 *        table and feature (registers, bits, a ramp, file records, a FIFO, identification and
 *        CANopen objects, a user-defined function code 0x41 with every action), so every
 *        function-code handler is reachable.
 *
 * The device is built once per process and fuzz inputs may change its state, as clients do.
 */
#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include "device/device.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "util/log.h"
#include "util/rng.h"
#include "vlib/arena.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Stops the fuzzer with a crash report when an invariant breaks. */
#define FUZZ_CHECK(cond)	do { if (!(cond)) __builtin_trap(); } while (0)

static device_t*
fuzz_device(void)
{
	static device_t*	dev;
	static const u16	file_values[] = { 0x1234, 0x5678, 0x9ABC, 0xDEF0 };
	static const u16	fifo_values[] = { 1, 2, 3 };
	static const u8		sdo[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
	arena_t*		arena;
	point_t			p = { .kind = POINT_STATIC };

	if (dev)
		return dev;
	log_set_level(LOG_LEVEL_OFF);
	arena = ARENA_ALLOC();
	dev   = device_new(arena, 0);
	FUZZ_CHECK(dev != NULL);
	dev->unit_id = 1;
	p.space = SPACE_HOLDING;
	p.type  = VT_U16;
	for (u32 a = 0; a < 16; a++)
	{
		p.address      = a;
		p.static_value = a * 11;
		FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	}
	p.address = 100;
	p.type    = VT_F32;
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.space = SPACE_INPUT;
	p.type  = VT_S32;
	p.kind  = POINT_RAMP;
	p.ramp  = (ramp_params_t){ .from = -50, .to = 50, .step = 5, .every_ms = 10, .mode = RAMP_BOUNCE };
	p.address = 0;
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.kind    = POINT_MODEL;
	p.type    = VT_S16;
	p.address = 10;
	p.model   = (model_t){ .shape = MODEL_SINE, .offset = 10, .amplitude = 5, .period_ms = 1000 };
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.type    = VT_F32;
	p.address = 12;
	p.model   = (model_t){ .shape = MODEL_LAG, .gain = 1, .tau_ms = 500, .target_is_point = true,
			       .target_space = SPACE_HOLDING, .target_address = 3,
			       .noise = { .amplitude = 1, .every_ms = 10, .gaussian = true } };
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.type    = VT_U16;
	p.address = 20;
	p.static_value = 500;
	p.model   = (model_t){ .degradation = { .after_ms = 5, .drift_per_hour = 3.6e6,
						.noise_growth_per_hour = 3.6e6, .has_stuck = true,
						.stuck_after_ms = 50, .has_fail = true, .fail_after_ms = 200 } };
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.space   = SPACE_HOLDING;
	p.address = 200;
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.space        = SPACE_COIL;
	p.type         = VT_BOOL;
	p.address      = 30;
	p.static_value = 0;
	p.model        = (model_t){ .degradation = { .has_fail = true, .fail_after_ms = 100 } };
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	device_reset_models(dev, 0);
	p = (point_t){ .space = SPACE_HOLDING, .kind = POINT_STATIC, .type = VT_U64, .address = 300,
		       .static_value = 1e15 };
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.type         = VT_F64;
	p.address      = 304;
	p.order        = WORDS_LOW_FIRST;
	p.byte_order   = BYTES_LITTLE;
	p.static_value = -2.5;
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.type         = VT_S64;
	p.address      = 308;
	p.static_exact = true;
	p.static_raw   = (u64)INT64_MIN;
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p = (point_t){ .space = SPACE_HOLDING, .kind = POINT_STATIC, .type = VT_STRING, .address = 320,
		       .length = 5, .text = "mogbus" };
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.space      = SPACE_INPUT;
	p.byte_order = BYTES_LITTLE;
	p.padding    = ' ';
	FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	p.kind  = POINT_STATIC;
	p.type  = VT_BOOL;
	p.order = WORDS_HIGH_FIRST;
	p.byte_order = BYTES_BIG;
	p.static_exact = false;
	p.length = 0;
	p.text = NULL;
	p.padding = 0;
	p.model = (model_t){ 0 };
	for (u32 a = 0; a < 24; a++)
	{
		p.space   = SPACE_COIL;
		p.address = a;
		FUZZ_CHECK(device_add_point(dev, &p) == NULL);
		p.space   = SPACE_DISCRETE;
		FUZZ_CHECK(device_add_point(dev, &p) == NULL);
	}
	FUZZ_CHECK(device_add_file_record(dev, 1, 0, 4, file_values) == NULL);
	FUZZ_CHECK(device_add_fifo(dev, 5, 3, fifo_values) == NULL);
	FUZZ_CHECK(device_add_canopen_object(dev, 1, 0x6041, 0, sizeof(sdo), sdo) == NULL);
	{
		static device_custom_fc_t fc = { .code = 0x41, .case_count = 6 };

		fc.cases[0] = (struct device_custom_case_t){ .prefix = { 1 }, .prefix_len = 1,
			.action = CUSTOM_REPLY, .reply = { 0x12, 0x34 }, .reply_len = 2 };
		fc.cases[1] = (struct device_custom_case_t){ .prefix = { 2 }, .prefix_len = 1,
			.action = CUSTOM_REGISTERS, .space = SPACE_HOLDING, .address = 0, .count = 16 };
		fc.cases[2] = (struct device_custom_case_t){ .prefix = { 3 }, .prefix_len = 1,
			.action = CUSTOM_REGISTERS, .space = SPACE_COIL, .address = 0, .count = 24 };
		fc.cases[3] = (struct device_custom_case_t){ .prefix = { 4, 4 }, .prefix_len = 2,
			.action = CUSTOM_ECHO };
		fc.cases[4] = (struct device_custom_case_t){ .prefix = { 5 }, .prefix_len = 1,
			.action = CUSTOM_NO_RESPONSE };
		fc.cases[5] = (struct device_custom_case_t){ .action = CUSTOM_EXCEPTION, .exception_code = 4 };
		FUZZ_CHECK(device_add_custom_fc(dev, &fc) == NULL);
	}
	snprintf(dev->name, sizeof(dev->name), "fuzz device");
	return dev;
}

static rng_t*
fuzz_rng(void)
{
	static rng_t rng;
	static bool  seeded;

	if (!seeded)
	{
		rng_seed(&rng, 1);
		seeded = true;
	}
	return &rng;
}

#endif
