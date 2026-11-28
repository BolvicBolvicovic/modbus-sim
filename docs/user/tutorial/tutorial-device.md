# Step 1: Build the device

> **Summary:** Describe the pumping-station device: its context, unit id and points of every kind, a read-only string, a ramp, a clock field, an event log and a FIFO, in a config file or in code.

A client sees a device only through four tables of 65536 addresses (coils, discrete inputs, input
registers, holding registers) plus, for the function codes that are not tables, file records and FIFO
queues. Only the points you configure exist: every other address is undefined, and a client that
touches it gets exception 02. A device is reached through its unit id, the address a client puts in its
requests.

## Points

Start with the points that never move.

```scheme
(server (mode tcp) (port 1502))

(device
  (name "Pump station")
  (unit-id 1)

  (holding 0 (type float32) (static 2.5))      ; two registers, high word first
  (holding 2 (type u16) (static 1))
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))
  (coil 0)
  (discrete 0)
  (discrete 1)
  (discrete 2)
  (input 0 (type float32) (static 2.5)))
```

```c
/* in main() */
if (mogbus_create(&ctx) != MOGBUS_OK)
...
mogbus_result_t error = mogbus_add_device(ctx, unit, "Pump station", &st->dev);
...
{ .space = MOGBUS_HOLDING, .address = HOLD_SETPOINT, .type = MOGBUS_TYPE_F32, .value = 2.5 },
{ .space = MOGBUS_HOLDING, .address = HOLD_MODE, .type = MOGBUS_TYPE_U16, .value = MODE_AUTO },
{ .space = MOGBUS_COIL, .address = COIL_RUN, .type = MOGBUS_TYPE_BOOL },
{ .space = MOGBUS_INPUT, .address = IN_RUN_SECONDS, .type = MOGBUS_TYPE_U32 },
```

```python
def build_station(ctx):
    dev = ctx.add_device(1, "Pump station")

    dev.add_point("holding", SETPOINT, "f32", 2.5)                      # level setpoint, m
    dev.add_point("holding", MODE, "u16", AUTO)
    dev.add_point("holding", SITE_NAME, "string", length=8, text="NORTH-01", read_only=True)
    dev.add_point("coil", RUN_COMMAND, "bool", 0)                       # pump run command (manual mode)
    for address in (HIGH_ALARM, LOW_ALARM, RUNNING):
        dev.add_point("discrete", address, "bool", 0)
    dev.add_point("input", LEVEL, "f32", 2.0)                           # tank level, m
    ...
    return dev
```

- **Config.** A config is a list of `(name value)` entries (see the [config format](../config/format.md)). A [`(type float32)`](../config/points/type.md) point spans two registers, [high word first](../config/points/word-order.md) unless you say otherwise; a point with no behaviour is a static point with value 0 (or off).
- **C.** Everything the library allocates belongs to a context (@ref mogbus_create "mogbus_create()", released with @ref mogbus_destroy "mogbus_destroy()"), and a context holds up to `MOGBUS_MAX_DEVICES` devices. A point is a @ref mogbus_point_t "mogbus_point_t": zero-initialise it and set what you need (a zeroed point is a static 0 of type u16 in the coil table, so bit points need `type = MOGBUS_TYPE_BOOL`). The program lists the station's points in an array of designated initializers and adds them in a loop with @ref mogbus_device_add_point "mogbus_device_add_point()". `HOLD_SETPOINT` and the other names are the program's enum of addresses. Every fallible call returns a @ref mogbus_result_t "mogbus_result_t" and the text of a failure is @ref mogbus_last_error "mogbus_last_error(ctx)".
- **Python.** \ref mogbus::core::Context "Context" owns devices and servers (use it as a context manager so everything is released on exit); \ref mogbus::core::Context::add_device "Context.add_device()" returns a \ref mogbus::core::Device "Device" and \ref mogbus::core::Device::add_point "Device.add_point()" adds points. Spaces, types and orders are plain strings (the enum members work as well) and a wrong name raises \ref mogbus::core::MogbusError "MogbusError" listing the valid ones. `SETPOINT`, `MODE` and the other names are constants of the example for the addresses.
- A float32 or u32 occupies two registers, so the setpoint (holding 0-1) and the mode (holding 2) sit side by side; overlaps and addresses above 65535 are rejected.

## A read-only string

The site name is a string point: N registers of two ASCII characters, in the holding or input table.
It is the third entry above, and it is read-only: a client write that touches it fails with exception
02 (see [`(access read-only)`](../config/points/access.md); only coils and holding registers take it,
input registers and discrete inputs are always read-only to clients). The host can still change a
read-only point, which is how the station will update what clients must not write ([step 3](tutorial-simulate.md)).

C and Python do the same with `.text`, `.length` and `.read_only` / `text=`, `length=` and `read_only=True`:

```c
{ .space = MOGBUS_HOLDING, .address = HOLD_SITE, .type = MOGBUS_TYPE_STRING, .length = 8,
  .text = "NORTH-01", .read_only = true },
```

```python
dev.add_point("holding", SITE_NAME, "string", length=8, text="NORTH-01", read_only=True)
```

## Ramps and clock fields

Not everything needs code to move it. A *ramp* moves a value by itself, evaluated when it is read, so
a client sees a live-looking value without anything of yours running; a *clock* point shows a field of
the wall clock. In the C program the inlet temperature (input 6) climbs from 12 to 18 degrees in steps
of 0.5 every second and bounces back, and input 20 shows the current minute of the hour in UTC; the
Python program has a bouncing inlet flow at input 6; the config tutorial has a clock at input 20 with
unix seconds. A ramp or a clock point stops being automatic when something writes it: a client write
turns a ramp static, and a host write makes it host-owned.

```scheme
(input 6 (type float32) (ramp (from 12) (to 18) (step 0.5) (every-ms 1000) (mode bounce)))
(input 20 (type u32) (clock unix))
```

```c
{ .space = MOGBUS_INPUT, .address = IN_INLET_TEMP, .type = MOGBUS_TYPE_F32, .kind = MOGBUS_POINT_RAMP,
  .ramp = { .from = 12, .to = 18, .step = 0.5, .every_ms = 1000, .mode = MOGBUS_RAMP_BOUNCE } },
{ .space = MOGBUS_INPUT, .address = IN_CLOCK_MINUTE, .type = MOGBUS_TYPE_U16, .kind = MOGBUS_POINT_CLOCK,
  .clock_field = MOGBUS_CLOCK_MINUTE, .clock_utc = true },
```

```python
dev.add_point("input", INFLOW, "f32", word_order="low-first",
              ramp={"from": 20, "to": 60, "step": 4, "every_ms": 1000, "mode": "bounce"})
```

- [`ramp`](../config/behaviours/ramp.md) modes are `wrap`, `bounce` and `hold`; [`clock`](../config/behaviours/clock.md) needs an integer type (`unix` needs 32 bits).
- `word_order="low-first"` makes the float32 send its low register first, the way some legacy meters do; `byte_order="little"` swaps the bytes of every register as well, and together they give the four classic layouts ([word order](../config/points/word-order.md), [byte order](../config/points/byte-order.md)).
- Machines (state machines that drive points) and faults exist only in config files, so a code-built device cannot have them; [step 3](tutorial-simulate.md) and [step 5](tutorial-faults.md) say what to do instead.

## File records and a FIFO

Two more stores serve function codes that are not tables: *file records* (clients read them with
function code 0x14 and write them with 0x15) and *FIFO queues* (read with 0x18). The station keeps its
event log in file 1 and a history in a FIFO; [step 4](tutorial-guard.md) fills and guards them.

```scheme
(file 1 (record 1 0 0 0 0 0 0 0 0 0 0))
(fifo 0 (values))
```

```c
error = mogbus_device_add_file_record(st->dev, LOG_FILE, 1, LOG_RECORDS, zeros);
return error != MOGBUS_OK ? error : mogbus_device_add_fifo(st->dev, FIFO_ADDRESS, 0, NULL);
```

```python
dev.add_file_record(EVENT_FILE, 0, [0] * EVENT_RECORDS)
dev.add_fifo(ALARM_FIFO, [])
```

In a config, file records and FIFO contents are fixed data ([`file`](../config/device/file.md),
[`fifo`](../config/device/fifo.md)); only the host API can change them while the station runs. The
C and Python arguments are the file number, the first record and its initial values, and the pointer
address and initial values of the queue; a FIFO holds up to 31 values.

## Check what you built

```bash
./mogbus --check=verbose station.scm
```

```c
mogbus_device_info_t info;

mogbus_device_info(st.dev, &info);
printf("device \"%s\", unit %u: %u holding, %u input, %u coil, %u discrete points\n", info.name,
       info.unit_id, info.point_count[MOGBUS_HOLDING], info.point_count[MOGBUS_INPUT],
       info.point_count[MOGBUS_COIL], info.point_count[MOGBUS_DISCRETE]);
```

```python
ctx = mogbus.Context()
dev = build_station(ctx)
info = dev.info()
print(f"{info.name!r}, unit {info.unit_id}, points per table: {info.point_counts}")
print("site name from the host:", dev.get_string("holding", SITE_NAME))
try:
    dev.add_point("holding", 1, "u16", 0)      # overlaps the setpoint's second register
except mogbus.MogbusError as exc:
    print("rejected:", exc)
```

With `station.scm` the first file of this step (save it under that name), `--check=verbose` prints the
same summary as a real start and validates everything it can without opening a socket (plain `--check`
prints nothing when the file is valid; a mistake is reported with its position, see [config
errors](../config/errors.md)):

```text
1 device on 127.0.0.1:1502
  unit 1   Pump station                     coils 1, discrete 3, input 1, holding 3, fault: none
config OK
```

The C program prints one line (holding counts three points, input six, coil one and discrete three):

```text
device "Pump station", unit 1: 3 holding, 6 input, 1 coil, 3 discrete points
```

and the Python program also shows that overlapping points are rejected:

```text
'Pump station', unit 1, points per table: {'coil': 1, 'discrete': 3, 'input': 5, 'holding': 3}
site name from the host: NORTH-01
rejected: address is already used by another point
```

\ref mogbus::core::Device::info "Device.info()" and @ref mogbus_device_info "mogbus_device_info()" return
a snapshot (name, unit id, points per table, faults, function codes); @ref mogbus_find_device "mogbus_find_device()",
@ref mogbus_device_count "mogbus_device_count()" and @ref mogbus_device_at "mogbus_device_at()" look
devices up.

## From a config file, in code

A device does not have to be built in code. @ref mogbus_load_config "mogbus_load_config()" and
\ref mogbus::core::Context::load_config "Context.load_config()" add the devices of a config file to a
context (on failure the context is unchanged), and `mogbus_config_server_options()` /
`Context.config_server_options()` give the file's `(server ...)` section as options for [step 2](tutorial-serve.md). A config
can describe what the API cannot build (machines, faults, user-defined function codes matched by
pattern), and the host side (`set_value`, hooks) still works on the devices it loads, so a profile and
a few lines of code often replace a whole model. Code-built and loaded devices can share a context. The
C program takes `--config FILE`; the Python helper \ref mogbus::core::serve "serve()" loads, serves and
cleans up in one call (`port=0` asks for a free port):

```c
if (mogbus_load_config(ctx, config) != MOGBUS_OK) { ... }
printf("loaded %s: %zu device(s)\n", config, mogbus_device_count(ctx));
if (mogbus_config_server_options(ctx, &from_config) == MOGBUS_OK)
    options = from_config;       /* the file's (server ...) section as options */
```

```python
with mogbus.serve(config_text='(device (unit-id 1) (holding 0 (static 7)))', port=0) as srv:
    with Client(srv.port) as plc:
        print("config_text:", plc.holding(0))

with mogbus.serve(config=str(ROOT / "profiles" / "flow-meter.scm"), port=0) as srv:
    srv.device(1).set_value("input", 0, 12.5)
    with Client(srv.port) as meter:
        print("flow meter from profiles/flow-meter.scm:", meter.read_f32(4, 0), "m3/h")
```

The C program, run with the two-device profile shipped in the repository and moved to unit 10 because
the profile already owns units 1 and 2, prints:

```bash
./pump_station --self-test --unit 10 --config profiles/two-devices.scm
```

```text
loaded profiles/two-devices.scm: 2 device(s)
device "Pump station", unit 10: 3 holding, 6 input, 1 coil, 3 discrete points
serving 3 device(s) on 127.0.0.1:41347
```

and the Python program:

```text
config_text: [7]
flow meter from profiles/flow-meter.scm: 12.5 m3/h
```

More on the API side: [points in C](../library/library-device.md), [points in Python](../python/python-device.md).

## Next

- [Step 2: Serve it and read it](tutorial-serve.md)
