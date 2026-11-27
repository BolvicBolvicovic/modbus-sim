# Step 1: Build the device

> **Summary:** Create a context, add the pumping-station device to it and describe its points, strings, ramps, a clock field, an event log and a FIFO, either in code or from a config file.

## The program's state

The tutorial program keeps what it needs in one struct, `struct station`. This step uses only its first field; the others are introduced in the steps that need them:

```c
struct station
{
	mogbus_device_t*	dev;		/* the device built in this step */
	double			level;		/* tank level, m (step 3) */
	...
	unsigned		refused;	/* writes vetoed by the hook (step 4) */
};
```

`dev` is the handle of the pumping-station device, a `mogbus_device_t*` that the library gives out when a device is added. Every later call that reads or changes the station takes it. The program also names every address of the scenario in an enum, so the code says `HOLD_SETPOINT` instead of `0`:

```c
enum
{
	HOLD_SETPOINT = 0,	/* holding 0-1: float32, metres */
	HOLD_MODE     = 2,	/* holding 2: u16 */
	HOLD_SITE     = 10,	/* holding 10-17: string of 8 registers */
	COIL_RUN      = 0,
	...
};
```

These are the addresses of the table in [the job](library-start.md#the-job). The function `build_device(ctx, st, unit)` fills a `struct station` and returns a status code; the sections below quote its parts.

## Context and device

Everything the library allocates belongs to a context, created with @ref mogbus_create "mogbus_create()" and released with @ref mogbus_destroy "mogbus_destroy()". A context holds up to `MOGBUS_MAX_DEVICES` devices; each device has one unit id, the address a client puts in its requests. Memory comes from an arena owned by the context, so there is no per-device free and no `malloc` in the library.

`main()` creates the context, and `build_device()` adds the device to it and stores the handle in `st->dev` (`unit` is 1 unless `--unit` says otherwise):

```c
mogbus_t* ctx = mogbus_create();    /* in main() */
...
st->dev = mogbus_add_device(ctx, unit, "Pump station");
```

@ref mogbus_add_device "mogbus_add_device()" returns NULL on a duplicate unit id. The reason is kept by the context: @ref mogbus_last_error_code "mogbus_last_error_code(ctx)" and @ref mogbus_last_error "mogbus_last_error(ctx)". Calls that return a @ref mogbus_error_t "mogbus_error_t" (`mogbus_load_config`, `mogbus_device_add_point`, ...) return the code directly and leave only the text in `mogbus_last_error()`; that is why `build_device()` returns `mogbus_last_error_code(ctx)` when the device is NULL.

## Points

A point is a @ref mogbus_point_t "mogbus_point_t": zero-initialise it and set what you need. A zeroed point is a static 0 of type u16 in the coil table, so bit points need `type = MOGBUS_TYPE_BOOL`. `build_device()` lists the station's points in an array of designated initializers and adds them in a loop with @ref mogbus_device_add_point "mogbus_device_add_point()". These four entries are the setpoint (a float32 starting at 2.5), the mode (a u16 starting at auto), the run command (a coil) and the run seconds (a u32):

```c
{ .space = MOGBUS_HOLDING, .address = HOLD_SETPOINT, .type = MOGBUS_TYPE_F32, .value = 2.5 },
{ .space = MOGBUS_HOLDING, .address = HOLD_MODE, .type = MOGBUS_TYPE_U16, .value = MODE_AUTO },
{ .space = MOGBUS_COIL, .address = COIL_RUN, .type = MOGBUS_TYPE_BOOL },
{ .space = MOGBUS_INPUT, .address = IN_RUN_SECONDS, .type = MOGBUS_TYPE_U32 },
```

`MODE_AUTO` is the program's own constant for mode 1. The loop that adds the whole array stops at the first failure:

```c
for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++)
{
	error = mogbus_device_add_point(st->dev, &points[i]);
	if (error != MOGBUS_OK)
		return error;
}
```

A float32 or u32 occupies two registers, so setpoint (holding 0-1) and mode (holding 2) sit side by side without overlapping; the library rejects overlaps and addresses above 65535. The types are `U16`, `S16`, `U32`, `S32`, `F32`, `BOOL`, `U64`, `S64`, `F64` and `STRING`.

- `word_order` (32- and 64-bit types) says which register holds the most significant part; `byte_order` swaps the two bytes of every register. Together they give the ABCD, CDAB, BADC and DCBA layouts tabulated in @ref mogbus_point_t "the point struct". The station uses the defaults (big-endian words and bytes, the common case).
- Decoded access goes through a `double`, so `u64` and `s64` above 2^53 round in `value`, @ref mogbus_device_get_value "mogbus_device_get_value()" and ramps; the raw register functions are exact.
- Code built against an older header keeps working when it zero-initialises the struct, because fields were only ever appended.

## A read-only string

The site name is a string point: `length` registers of two ASCII characters, holding or input table only, initial `text` copied (at most `2 * length` bytes, NUL padded). It is one more entry of the same array, with `read_only` set:

```c
{ .space = MOGBUS_HOLDING, .address = HOLD_SITE, .type = MOGBUS_TYPE_STRING, .length = 8,
  .text = "NORTH-01", .read_only = true },
```

`read_only` (coils and holding registers only) makes every Modbus write that touches the point fail with exception 02, the same as `(access read-only)` in a config. The self-test (a mode of the tutorial program that sends requests to its own server, see [Run it](library-start.md#run-it)) tries to write the site name with function code 16 and prints:

```text
  ok   FC 16 on the read-only site name: exception 02
```

Host code can still change a read-only point, which is what lets the station update things clients must not write; [step 3](library-simulate.md) shows how.

## Ramps and clock fields

Not everything needs the host to move it. A point of `kind = MOGBUS_POINT_RAMP` follows a ramp over time, evaluated when it is read, and `MOGBUS_POINT_CLOCK` shows a field of the wall clock. In the station, the inlet temperature (input 6) climbs from 12 to 18 degrees in steps of 0.5 every second and bounces back, and input 20 shows the current minute of the hour in UTC:

```c
{ .space = MOGBUS_INPUT, .address = IN_INLET_TEMP, .type = MOGBUS_TYPE_F32, .kind = MOGBUS_POINT_RAMP,
  .ramp = { .from = 12, .to = 18, .step = 0.5, .every_ms = 1000, .mode = MOGBUS_RAMP_BOUNCE } },
{ .space = MOGBUS_INPUT, .address = IN_CLOCK_MINUTE, .type = MOGBUS_TYPE_U16, .kind = MOGBUS_POINT_CLOCK,
  .clock_field = MOGBUS_CLOCK_MINUTE, .clock_utc = true },
```

`mode` is `MOGBUS_RAMP_WRAP`, `BOUNCE` or `HOLD`. A clock point needs an integer register type (`MOGBUS_CLOCK_UNIX` needs 32 bits). Both stop being automatic if something writes them: a client write turns a ramp into a static value, and a host @ref mogbus_device_set_value "mogbus_device_set_value()" makes it host-owned. Machines (state machines that drive points) and faults exist only in config files; a code-built device and a config-loaded one can live in the same context, see the last section.

## File records and a FIFO

Two more stores serve function codes that are not tables. At the end of `build_device()`, the program adds the event log as file 1 with records 1 to 100 (`zeros` is an array of 100 zero registers, the initial content), and a FIFO queue at pointer address 0 for the level history:

```c
error = mogbus_device_add_file_record(st->dev, LOG_FILE, 1, LOG_RECORDS, zeros);
return error != MOGBUS_OK ? error : mogbus_device_add_fifo(st->dev, FIFO_ADDRESS, 0, NULL);
```

@ref mogbus_device_add_file_record "mogbus_device_add_file_record()" adds the file records (clients read them with FC 14 and write them with FC 15), and @ref mogbus_device_add_fifo "mogbus_device_add_fifo()" adds the queue (clients read it with FC 18); the last two arguments are the initial number of values and their array, here none. A file holds records numbered up to 9999, added in ranges of 1 to 126 records that must not overlap (at most 16 files and 64 ranges per file); a FIFO holds up to 31 values, and storage always has room for all 31 so it can grow later. CANopen objects for FC 2B/0D work the same way with @ref mogbus_device_add_canopen_object "mogbus_device_add_canopen_object()". [Step 4](library-hooks.md) fills and guards these.

## Check what you built

@ref mogbus_device_info "mogbus_device_info()" fills a snapshot: name, unit id, points per table, configured faults and user-defined function codes. After `build_device()`, `main()` prints it:

```c
mogbus_device_info_t info;

mogbus_device_info(st.dev, &info);
printf("device \"%s\", unit %u: %u holding, %u input, %u coil, %u discrete points\n", info.name,
       info.unit_id, info.point_count[MOGBUS_HOLDING], info.point_count[MOGBUS_INPUT],
       info.point_count[MOGBUS_COIL], info.point_count[MOGBUS_DISCRETE]);
```

```text
device "Pump station", unit 1: 3 holding, 6 input, 1 coil, 3 discrete points
```

Holding counts three points (setpoint, mode, site name), input six, coil one and discrete three. @ref mogbus_find_device "mogbus_find_device()", @ref mogbus_device_count "mogbus_device_count()" and @ref mogbus_device_at "mogbus_device_at()" look devices up or enumerate them in order.

## Building the device from a config file

The same station can be written as a config file (see [the app tutorial](../tutorial-app.md)). @ref mogbus_load_config "mogbus_load_config()" adds the devices of a file to the context and remembers its `(server ...)` section; on failure the context is unchanged and `mogbus_last_error(ctx)` says why. A config can describe what the API cannot build: machines, faults and user-defined function codes matched by request pattern. Code-built and loaded devices can be mixed in one context.

The tutorial program takes `--config FILE`. `main()` loads the file before it builds the station, so the station is added next to the devices of the file. In the excerpt, `config` is the path from the command line, `options` the server options the program will use (step 2) and `from_config` a temporary of the same type:

```c
if (mogbus_load_config(ctx, config) != MOGBUS_OK) { ... }
printf("loaded %s: %zu device(s)\n", config, mogbus_device_count(ctx));
if (mogbus_config_server_options(ctx, &from_config))
    options = from_config;       /* the file's (server ...) section as options */
```

@ref mogbus_config_server_options "mogbus_config_server_options()" returns false when the file has no `(server ...)` section; otherwise it fills a @ref mogbus_server_options_t "mogbus_server_options_t", so a harness can reuse the transport settings of a profile. Run the self-test with the two-device profile shipped in the repository, and move the station to unit 10 because the profile already owns units 1 and 2:

```bash
./pump_station --self-test --unit 10 --config profiles/two-devices.scm
```

```text
loaded profiles/two-devices.scm: 2 device(s)
device "Pump station", unit 10: 3 holding, 6 input, 1 coil, 3 discrete points
serving 3 device(s) on 127.0.0.1:41347
```

@ref mogbus_device_set_any_unit "mogbus_device_set_any_unit()" makes one device answer every unit id nobody else has, like `(any-unit on)`.

## Next

- [Step 2: Serve it from your own loop](library-serve.md)
