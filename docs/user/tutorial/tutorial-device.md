# Step 1: Build the device

> **Summary:** Describe the pump station device, with its unit id and a point for every register, coil and record of the table in the tutorial start page, in a config file or in code, and check what you built.

A client sees a device only through four tables of up to 65536 addresses each (coils, discrete inputs,
input registers, holding registers) plus, for the function codes that are not tables, file records and
FIFO queues. Only the points you describe exist: every other address is undefined, and a client that
touches it gets Modbus exception 02 (illegal data address). A device is reached through its unit id, the
address a client puts in its requests.

At the end of this step you have the whole station of the [start page](tutorial-start.md#the-station),
as a file `station.scm` and as a program that builds it in code. The later steps load `station.scm`, so
save it even if you follow the C or Python route.

## Describe the points

Each line below is one point: the table, the first address, the type and the value the point starts with.
A float32 or u32 takes two registers (the high word comes first, the usual layout), so the setpoint
occupies holding 0 and 1 and the mode sits at holding 2. The site name is a string of 8 registers, two
characters each, and read-only: a client that writes it gets exception 02.

```scheme
; station.scm - the pump station: one device with Modbus unit id 1.
; Addresses are protocol addresses, counted from 0: address 0 of the holding table is the
; register a PLC manual calls 40001.

(server (mode tcp) (port 1502))

(device
  (name "Pump station")
  (unit-id 1)

  ; Holding registers: a client can read and write them.
  (holding 0 (type float32) (static 2.5))      ; level setpoint in metres (registers 0 and 1)
  (holding 2 (type u16) (static 1))            ; mode: 0 off, 1 auto, 2 manual
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))  ; site name

  ; Coil: one bit a client can switch.
  (coil 0)                                     ; pump run command, used in manual mode

  ; Discrete inputs: bits a client can only read.
  (discrete 0)                                 ; high-level alarm
  (discrete 1)                                 ; low-level alarm
  (discrete 2)                                 ; pump running

  ; Input registers: measurements a client can only read.
  (input 0 (type float32) (static 2.5))        ; tank level in metres (registers 0 and 1)
  (input 2 (type float32) (static 40))         ; outflow in m3/h (registers 2 and 3)
  (input 4 (type u16) (static 0))              ; motor current in tenths of an ampere
  (input 10 (type u32) (static 0))             ; pump run time in seconds (registers 10 and 11)

  ; Inlet temperature in degrees C (registers 6 and 7): it moves by itself from 12 to 18 and
  ; back, 0.5 degrees every second, without any code of yours.
  (input 6 (type float32) (ramp (from 12) (to 18) (step 0.5) (every-ms 1000) (mode bounce)))

  ; Wall clock (registers 20 and 21): seconds since 1 January 1970.
  (input 20 (type u32) (clock unix))

  ; Event log, read and written with function codes 0x14 and 0x15: file 1 has the records 0 and 1.
  (file 1 (record 0 0 0))

  ; Alarm history, read with function code 0x18: FIFO 0, empty to start with.
  (fifo 0 (values)))
```

```c
/* step1.c - build the pump station in code and print what was built. */
#include <stdio.h>
#include "mogbus.h"

int
main(void)
{
	mogbus_t* ctx = NULL;        /* owns everything the library allocates */
	mogbus_device_t* dev = NULL; /* the pump station, Modbus unit id 1 */

	if (mogbus_create(&ctx) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(NULL));
		return 1;
	}
	if (mogbus_add_device(ctx, 1, "Pump station", &dev) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	/* One entry per point: the table, the first address, the type and the start value.
	 * A float32 or a u32 uses two registers; the string uses 8 registers of two characters. */
	mogbus_point_t points[] = {
		{ .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_F32, .value = 2.5 },  /* setpoint, m */
		{ .space = MOGBUS_HOLDING, .address = 2, .type = MOGBUS_TYPE_U16, .value = 1 },    /* mode */
		{ .space = MOGBUS_HOLDING, .address = 10, .type = MOGBUS_TYPE_STRING, .length = 8,
		  .text = "NORTH-01", .read_only = true },                                         /* site name */
		{ .space = MOGBUS_COIL, .address = 0, .type = MOGBUS_TYPE_BOOL },                  /* run command */
		{ .space = MOGBUS_DISCRETE, .address = 0, .type = MOGBUS_TYPE_BOOL },              /* high alarm */
		{ .space = MOGBUS_DISCRETE, .address = 1, .type = MOGBUS_TYPE_BOOL },              /* low alarm */
		{ .space = MOGBUS_DISCRETE, .address = 2, .type = MOGBUS_TYPE_BOOL },              /* pump running */
		{ .space = MOGBUS_INPUT, .address = 0, .type = MOGBUS_TYPE_F32, .value = 2.5 },    /* level, m */
		{ .space = MOGBUS_INPUT, .address = 2, .type = MOGBUS_TYPE_F32, .value = 40 },     /* outflow, m3/h */
		{ .space = MOGBUS_INPUT, .address = 4, .type = MOGBUS_TYPE_U16, .value = 0 },      /* current, 0.1 A */
		{ .space = MOGBUS_INPUT, .address = 10, .type = MOGBUS_TYPE_U32, .value = 0 },     /* run seconds */

		/* Inlet temperature in degrees C: it moves by itself from 12 to 18 and back, 0.5 degrees
		 * every second, without any code of yours. */
		{ .space = MOGBUS_INPUT, .address = 6, .type = MOGBUS_TYPE_F32, .kind = MOGBUS_POINT_RAMP,
		  .ramp = { .from = 12, .to = 18, .step = 0.5, .every_ms = 1000, .mode = MOGBUS_RAMP_BOUNCE } },
		/* Wall clock, seconds since 1 January 1970 (UTC). */
		{ .space = MOGBUS_INPUT, .address = 20, .type = MOGBUS_TYPE_U32, .kind = MOGBUS_POINT_CLOCK,
		  .clock_field = MOGBUS_CLOCK_UNIX },
	};
	size_t point_count = sizeof points / sizeof points[0];

	for (size_t i = 0; i < point_count; i++)
	{
		if (mogbus_device_add_point(dev, &points[i]) != MOGBUS_OK)
		{
			fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
			return 1;
		}
	}

	/* Event log, read and written with function codes 0x14 and 0x15: file 1 with records 0 and 1,
	 * both starting at 0. Alarm history, read with function code 0x18: FIFO 0, empty. */
	uint16_t event_log[2] = { 0, 0 };

	if (mogbus_device_add_file_record(dev, 1, 0, 2, event_log) != MOGBUS_OK
	    || mogbus_device_add_fifo(dev, 0, 0, NULL) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	mogbus_device_info_t info;

	mogbus_device_info(dev, &info);
	printf("device \"%s\", unit %u: %u holding, %u input, %u coil, %u discrete points\n",
	       info.name, (unsigned)info.unit_id, (unsigned)info.point_count[MOGBUS_HOLDING],
	       (unsigned)info.point_count[MOGBUS_INPUT], (unsigned)info.point_count[MOGBUS_COIL],
	       (unsigned)info.point_count[MOGBUS_DISCRETE]);

	mogbus_destroy(ctx);
	return 0;
}
```

```python
# step1.py - build the pump station in code and print what was built.
import mogbus

with mogbus.Context() as ctx:                # owns everything; leaving the block releases it
    dev = ctx.add_device(1, "Pump station")  # Modbus unit id 1

    # add_point(table, first address, type, start value)
    dev.add_point("holding", 0, "f32", 2.5)   # level setpoint in metres (registers 0 and 1)
    dev.add_point("holding", 2, "u16", 1)     # mode: 0 off, 1 auto, 2 manual
    dev.add_point("holding", 10, "string", length=8, text="NORTH-01", read_only=True)  # site name
    dev.add_point("coil", 0, "bool", 0)       # pump run command
    dev.add_point("discrete", 0, "bool", 0)   # high-level alarm
    dev.add_point("discrete", 1, "bool", 0)   # low-level alarm
    dev.add_point("discrete", 2, "bool", 0)   # pump running
    dev.add_point("input", 0, "f32", 2.5)     # tank level in metres (registers 0 and 1)
    dev.add_point("input", 2, "f32", 40)      # outflow in m3/h (registers 2 and 3)
    dev.add_point("input", 4, "u16", 0)       # motor current in tenths of an ampere
    dev.add_point("input", 10, "u32", 0)      # pump run time in seconds (registers 10 and 11)


    # Inlet temperature in degrees C: it moves by itself from 12 to 18 and back, 0.5 degrees
    # every second, without any code of yours.
    dev.add_point("input", 6, "f32",
                  ramp={"from": 12, "to": 18, "step": 0.5, "every_ms": 1000, "mode": "bounce"})
    # Wall clock, seconds since 1 January 1970 (UTC).
    dev.add_point("input", 20, "u32", clock="unix")

    # Event log, read and written with function codes 0x14 and 0x15: file 1 with records 0 and 1,
    # both starting at 0.
    dev.add_file_record(1, 0, [0, 0])
    # Alarm history, read with function code 0x18: FIFO 0, empty to start with.
    dev.add_fifo(0, [])

    info = dev.info()
    print(f"device {info.name!r}, unit {info.unit_id}, points per table: {info.point_counts}")
```

- **Scheme / Bash.** A config is a list of entries `(name value ...)`, see the [config format](../config/format.md). Text after a `;` is a comment. [`(type ...)`](../config/points/type.md) defaults to `u16`, a point with no behaviour is a static one, and [`(access read-only)`](../config/points/access.md) makes client writes fail.
- **C.** Everything the library allocates belongs to a context: @ref mogbus_create "mogbus_create()" makes it and @ref mogbus_destroy "mogbus_destroy()" releases it. A point is a @ref mogbus_point_t "mogbus_point_t". Fields you leave out are zero, which means a static value of 0; bits need `.type = MOGBUS_TYPE_BOOL`.
- **Python.** `mogbus.Context` is the context; leaving its `with` block releases everything. The type of a point is a name such as `"f32"`, `"u16"`, `"bool"` or `"string"`, and a wrong name raises `MogbusError` with the list of the valid ones.

Overlapping points and addresses above 65535 are rejected.

## Points that move by themselves, and records

The same programs also describe four points that need no code to be alive:

- The inlet temperature (input 6-7) is a *ramp*: it climbs from 12 to 18 degrees in steps of 0.5 every second, then comes back. The value is computed when a client reads it, so nothing of yours has to run ([`ramp`](../config/behaviours/ramp.md)).
- The wall clock (input 20-21) is a *clock* point: the seconds since 1970 ([`clock`](../config/behaviours/clock.md)).
- The event log is *file* 1, with the records 0 and 1. Clients read file records with function code 0x14 and write them with 0x15 ([`file`](../config/device/file.md)).
- The alarm history is the *FIFO* queue 0, which clients read with function code 0x18 ([`fifo`](../config/device/fifo.md)).

Some old meters send the two registers of a float32 low word first; that is a [word order](../config/points/word-order.md) setting of the point, with a [byte order](../config/points/byte-order.md) setting next to it.

## Check what you built

Run the program, or ask `mogbus` to read the config without opening a port:

```bash
./mogbus --check=verbose station.scm
```

```text
1 device on 127.0.0.1:1502
  unit 1   Pump station                     coils 1, discrete 3, input 6, holding 3, fault: none
config OK
```

The C program prints this line:

```text
device "Pump station", unit 1: 3 holding, 6 input, 1 coil, 3 discrete points
```

and the Python script this one:

```text
device 'Pump station', unit 1, points per table: {'coil': 1, 'discrete': 3, 'input': 6, 'holding': 3}
```

Plain `--check` prints nothing when the file is valid; a mistake is reported with its position, see
[config errors](../config/errors.md). The same facts come from @ref mogbus_device_info "mogbus_device_info()"
and \ref mogbus::core::Device::info "Device.info()".

## Load a config in code

A program does not have to build a device by hand: @ref mogbus_load_config "mogbus_load_config()" and
\ref mogbus::core::Context::load_config "Context.load_config()" add the devices of a config file to a
context. The next steps do this with `station.scm`, so the programs stay short. A config can also
describe what the C and Python calls cannot build (machines, faults, ...), and the host side
(`set_value`, hooks) still works on the devices it loads.

More on the API side: [points in C](../library/library-device.md), [points in Python](../python/python-device.md).

## Next

- [Step 2: Serve it and read it](tutorial-serve.md)
