# Step 1: Build the device

Create a context, add the pumping-station device and describe its points, strings, ramps, a clock field, an event log and a FIFO.

Everything the library allocates belongs to a context (@ref mogbus_t "mogbus_t"), created with @ref mogbus_create "mogbus_create()" and released with @ref mogbus_destroy "mogbus_destroy()". A context holds up to `MOGBUS_MAX_DEVICES` devices; each device has one unit id. Memory comes from an arena owned by the context, so there is no per-device free and no `malloc` in the library.

```c
mogbus_t* ctx = mogbus_create();
...
st->dev = mogbus_add_device(ctx, unit, "Pump station");
```

@ref mogbus_add_device "mogbus_add_device()" returns NULL on a duplicate unit id. The reason is kept by the context: @ref mogbus_last_error_code "mogbus_last_error_code(ctx)" and @ref mogbus_last_error "mogbus_last_error(ctx)". Calls that return a @ref mogbus_error_t "mogbus_error_t" (`mogbus_load_config`, `mogbus_device_add_point`, ...) return the code directly and leave only the text in `mogbus_last_error()`.

## Points

A point is a @ref mogbus_point_t "mogbus_point_t": zero-initialise it and set what you need. A zeroed point is a static 0 of type u16 in the coil table, so bit points need `type = MOGBUS_TYPE_BOOL`. The station's points are an array of designated initializers added in a loop with @ref mogbus_device_add_point "mogbus_device_add_point()":

```c
{ .space = MOGBUS_HOLDING, .address = HOLD_SETPOINT, .type = MOGBUS_TYPE_F32, .value = 2.5 },
{ .space = MOGBUS_HOLDING, .address = HOLD_MODE, .type = MOGBUS_TYPE_U16, .value = MODE_AUTO },
{ .space = MOGBUS_COIL, .address = COIL_RUN, .type = MOGBUS_TYPE_BOOL },
{ .space = MOGBUS_INPUT, .address = IN_RUN_SECONDS, .type = MOGBUS_TYPE_U32 },
```

A float32 or u32 occupies two registers, so setpoint (holding 0-1) and mode (holding 2) sit side by side without overlapping; the library rejects overlaps and addresses above 65535. The types are `U16`, `S16`, `U32`, `S32`, `F32`, `BOOL`, `U64`, `S64`, `F64` and `STRING`.

- `word_order` (32- and 64-bit types) says which register holds the most significant part; `byte_order` swaps the two bytes of every register. Together they give the ABCD, CDAB, BADC and DCBA layouts tabulated in @ref mogbus_point_t "the point struct". The station uses the defaults (big-endian words and bytes, the common case).
- Decoded access goes through a `double`, so `u64` and `s64` above 2^53 round in `value`, @ref mogbus_device_get_value "mogbus_device_get_value()" and ramps; the raw register functions are exact.
- Code built against an older header keeps working when it zero-initialises the struct, because fields were only ever appended.

## A read-only string

The site name is a string point: `length` registers of two ASCII characters, holding or input table only, initial `text` copied (at most `2 * length` bytes, NUL padded):

```c
{ .space = MOGBUS_HOLDING, .address = HOLD_SITE, .type = MOGBUS_TYPE_STRING, .length = 8,
  .text = "NORTH-01", .read_only = true },
```

`read_only` (coils and holding registers only) makes every Modbus write that touches the point fail with exception 02, the same as `(access read-only)` in a config. The self-test proves it:

```text
  ok   FC 16 on the read-only site name: exception 02
```

Host code can still change a read-only point, which is what lets the station update things clients must not write; [step 3](library-simulate.md) shows how.

## Ramps and clock fields

Not everything needs the host to move it. A point of `kind = MOGBUS_POINT_RAMP` follows a ramp over time, evaluated when it is read, and `MOGBUS_POINT_CLOCK` shows a field of the wall clock:

```c
{ .space = MOGBUS_INPUT, .address = IN_INLET_TEMP, .type = MOGBUS_TYPE_F32, .kind = MOGBUS_POINT_RAMP,
  .ramp = { .from = 12, .to = 18, .step = 0.5, .every_ms = 1000, .mode = MOGBUS_RAMP_BOUNCE } },
{ .space = MOGBUS_INPUT, .address = IN_CLOCK_MINUTE, .type = MOGBUS_TYPE_U16, .kind = MOGBUS_POINT_CLOCK,
  .clock_field = MOGBUS_CLOCK_MINUTE, .clock_utc = true },
```

`mode` is `MOGBUS_RAMP_WRAP`, `BOUNCE` or `HOLD`. A clock point needs an integer register type (`MOGBUS_CLOCK_UNIX` needs 32 bits). Both stop being automatic if something writes them: a client write turns a ramp into a static value, and a host @ref mogbus_device_set_value "mogbus_device_set_value()" makes it host-owned. Machines (state machines that drive points), faults and user-defined function codes exist only in config files; code-built and loaded devices can be mixed in one context, see below.

## File records and a FIFO

Two more stores serve function codes that are not tables. The event log is file 1, records 1 to 100, added with @ref mogbus_device_add_file_record "mogbus_device_add_file_record()" (clients read it with FC 14 and write it with FC 15); the level history is a FIFO queue at pointer address 0 added with @ref mogbus_device_add_fifo "mogbus_device_add_fifo()" (clients read it with FC 18):

```c
error = mogbus_device_add_file_record(st->dev, LOG_FILE, 1, LOG_RECORDS, zeros);
return error != MOGBUS_OK ? error : mogbus_device_add_fifo(st->dev, FIFO_ADDRESS, 0, NULL);
```

A file holds records numbered up to 9999, added in ranges of 1 to 126 records that must not overlap (at most 16 files and 64 ranges per file); a FIFO holds up to 31 values, and storage always has room for all 31 so it can grow later. CANopen objects for FC 2B/0D work the same way with @ref mogbus_device_add_canopen_object "mogbus_device_add_canopen_object()". [Step 4](library-hooks.md) fills and guards these.

## Check what you built

@ref mogbus_device_info "mogbus_device_info()" fills a snapshot: name, unit id, points per table, configured faults and user-defined function codes. The program prints it:

```text
device "Pump station", unit 1: 3 holding, 6 input, 1 coil, 3 discrete points
```

Holding counts three points (setpoint, mode, site name), input six, coil one and discrete three. @ref mogbus_find_device "mogbus_find_device()", @ref mogbus_device_count "mogbus_device_count()" and @ref mogbus_device_at "mogbus_device_at()" look devices up or enumerate them in order.

## Loading a config instead, or as well

The same station can be written as a config file (see [the app tutorial](../tutorial-app.md)); @ref mogbus_load_config "mogbus_load_config()" adds the devices of a file and remembers its `(server ...)` section. On failure the context is unchanged and `mogbus_last_error(ctx)` says why. The example takes `--config FILE`, loads it first and then builds the station next to it:

```c
if (mogbus_load_config(ctx, config) != MOGBUS_OK) { ... }
printf("loaded %s: %zu device(s)\n", config, mogbus_device_count(ctx));
if (mogbus_config_server_options(ctx, &from_config))
    options = from_config;       /* the file's (server ...) section as options */
```

```bash
./examples/tutorial/pump_station_static --self-test --unit 10 --config profiles/two-devices.scm
```

```text
loaded profiles/two-devices.scm: 2 device(s)
device "Pump station", unit 10: 3 holding, 6 input, 1 coil, 3 discrete points
serving 3 device(s) on 127.0.0.1:41347
```

The station took unit 10 because `two-devices.scm` already owns units 1 and 2. A config adds what the API cannot build: machines, faults and user-defined function codes. @ref mogbus_config_server_options "mogbus_config_server_options()" returns the `(server ...)` section as @ref mogbus_server_options_t "mogbus_server_options_t", so a harness can reuse the transport settings of a profile. @ref mogbus_device_set_any_unit "mogbus_device_set_any_unit()" makes one device answer every unit id nobody else has, like `(any-unit on)`.

## Next

- [Step 2: Serve it from your own loop](library-serve.md)
