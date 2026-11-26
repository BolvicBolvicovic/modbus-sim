/**
 * @file
 * @brief Shared setup for the libFuzzer harnesses: one device that owns something in every
 *        table and feature (registers, bits, a ramp, file records, a FIFO, identification and
 *        CANopen objects), so every function-code handler is reachable.
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
	p.kind = POINT_STATIC;
	p.type = VT_BOOL;
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
