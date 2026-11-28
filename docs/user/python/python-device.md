# Devices and points in Python

Create a context, add the pumping-station device to it and describe its points, strings, a ramp, an event log and a FIFO, either in code or from a config file.

## Context and device

A \ref mogbus::core::Context "Context" owns devices and servers; use it as a context manager so
everything is released on exit. \ref mogbus::core::Context::add_device "Context.add_device()" takes a
unit id (the address a client puts in its requests) and a name and returns a
\ref mogbus::core::Device "Device", the handle every later call uses.
\ref mogbus::core::Device::add_point "Device.add_point()" adds points to the device's four tables. The
function `build_station(ctx)` of the example does all of it and returns the device; the code below is
its body. `SETPOINT`, `MODE` and the other capitalised names are the register addresses of
[the station table](../tutorial/tutorial-start.md#the-station):

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
    dev.add_point("input", OUTFLOW, "f32", 0.0)                         # m3/h
    dev.add_point("input", CURRENT, "u16", 0)                           # motor current, 0.1 A
    dev.add_point("input", RUN_SECONDS, "u32", 0)                       # pump run seconds
    dev.add_point("input", INFLOW, "f32", word_order="low-first",
                  ramp={"from": 20, "to": 60, "step": 4, "every_ms": 1000, "mode": "bounce"})
    ...
    return dev
```

Each call passes the table, the first address, the type and the start value. Things worth noticing:

- Spaces, types and orders are plain strings (`"holding"`, `"f32"`, `"low-first"`); the enum members
  (\ref mogbus::core::Space "Space", \ref mogbus::core::ValueType "ValueType") work as well, and a
  wrong name raises \ref mogbus::core::MogbusError "MogbusError" listing the valid ones.
- A point owns as many addresses as its type needs: the float32 setpoint is addresses 0 and 1.
  Only configured points exist; an address with no owner stays undefined and a client gets exception
  02 for it.
- Points are created with a start value; `u64` and `s64` start values are floats here, so integers
  above 2^53 round (\ref mogbus::core::Device::read "Device.read()" is always exact).

## A read-only string

The site name is a `string` point: `length` registers of two ASCII characters each, holding or input
table only, starting with `text`. The call is the third line of `build_station`; `read_only=True`
makes every Modbus write that touches the point fail with exception 02, while the host can still
change it with \ref mogbus::core::Device::set_string "Device.set_string()". The [tutorial](../tutorial/tutorial-serve.md) shows a client
trying to write it.

## A ramp

Not everything needs code to move it. The inlet flow is a *ramp*, the last point of `build_station`:
it moves by itself (20, 24, 28 ... 60, then back, one step per second), evaluated when it is read, so
a client sees a live-looking value without any code of yours running. `word_order="low-first"` makes
the float32 send its low register first, the way some legacy meters do; `byte_order="little"` would
swap the bytes of every register as well, and together they give the four classic layouts. A ramp
stops being automatic when something writes it: a client write turns it static, and a host
\ref mogbus::core::Device::set_value "Device.set_value()" makes it host-owned (the [tutorial](../tutorial/tutorial-simulate.md) pins it that
way).

## File records and a FIFO

Besides the four tables a device can hold *file records* (clients read them with function code 0x14
and write them with 0x15) and *FIFO queues* (read with 0x18). The elided end of `build_station` adds
an event log of ten records, one register each, in file 1, and an empty alarm queue at pointer
address 100:

```python
dev.add_file_record(EVENT_FILE, 0, [0] * EVENT_RECORDS)
dev.add_fifo(ALARM_FIFO, [])
```

The arguments are the file number, the first record and its initial values for
\ref mogbus::core::Device::add_file_record "Device.add_file_record()", and the pointer address and
the initial values for \ref mogbus::core::Device::add_fifo "Device.add_fifo()". [Hooks](python-hooks.md)
fills and guards them.

## Building the device from a config file

When a device does not need code, describe it as a config file and let \ref mogbus::core::serve "serve()"
do the rest: it creates a context, loads the config, serves it from a background thread and yields a
\ref mogbus::core::Served "Served" with the context, the server, the port and `device(unit_id)`. It
cleans up on exit. `config_text` takes the config as a string, `config` a path. In the example,
`Client` is the small Modbus client of the program, `srv.port` the port the server picked (`port=0`
asks for a free one) and `ROOT` the repository root, so the second call loads the
[`flow-meter` profile](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/flow-meter.scm):

```python
with mogbus.serve(config_text='(device (unit-id 1) (holding 0 (static 7)))', port=0) as srv:
    with Client(srv.port) as plc:
        print("config_text:", plc.holding(0))

with mogbus.serve(config=str(ROOT / "profiles" / "flow-meter.scm"), port=0) as srv:
    srv.device(1).set_value("input", 0, 12.5)
    with Client(srv.port) as meter:
        print("flow meter from profiles/flow-meter.scm:", meter.read_f32(4, 0), "m3/h")
```

```text
config_text: [7]
flow meter from profiles/flow-meter.scm: 12.5 m3/h
```

The second block also sets a value from the host on the loaded device, `srv.device(1).set_value(...)`,
the same call as for a device built in code. The server uses what the `mogbus` binary would use: the
config's own `(server ...)` section, or the defaults (TCP on 1502) when it has none. Only the keyword
arguments you pass override its fields (`mogbus.serve(config=path, port=0, mode="rtu-tcp",
idle_timeout_ms=500)`); `port=0` in a network mode picks a free loopback port, and without a config you
get TCP on a free port.

A config is where machines (a pump model written as expressions) and faults on schedules live, which
the Python API cannot create; the host side (`set_value()`, hooks) still works on the devices it loads,
so a profile and a few lines of Python often replace a whole model class. To mix both, load a file into
a context you already have with \ref mogbus::core::Context::load_config "Context.load_config()"; its
`(server ...)` section is then available from
\ref mogbus::core::Context::config_server_options "Context.config_server_options()", which [serving](python-serve.md) uses.

## See also

- [Devices and points in C](../library/library-device.md)
- [Tutorial step 1: Build the device](../tutorial/tutorial-device.md)
- \ref core.py "Python API reference"
