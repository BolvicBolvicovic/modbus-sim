# Python tutorial: simulate a pumping station

Build a simulated water pumping station in Python, serve it over Modbus TCP and test your own client code against it.

You maintain a SCADA screen, a data logger or some control code that talks Modbus to a pumping
station, and the real PLC is not on your desk (or you do not want a test to open its valves). In this
tutorial you build the station with the `mogbus` Python package, run it from a background thread,
make its process behave (the tank fills, the pump runs, alarms rise), enforce the rules of the real
controller, watch what your client does and finally turn it into automated tests. Every step runs: the
complete program is
[`examples/python/pump_station.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/pump_station.py),
and CI runs it, so the output shown here is real.

| Step | Page | What you do |
|---|---|---|
| 1 | this page | build the device with points of every kind you need |
| 2 | this page | serve it and read it with a client |
| 3-5 | [Driving the process from the host](python-process.md) | tank physics, the access hook, event log and alarm FIFO |
| 6 | [Threads, locks and shutdown](python-threading.md) | run the process model beside the server |
| 7-9 | [Logs, metrics, configs and runtime control](python-observe.md) | observe it, load profiles, other transports |
| tests | [Testing your client](python-testing.md) | unittest and pytest fixtures |

The names of the classes and methods link to the \ref core.py "Python API" reference, which is
generated from the docstrings of the package; this tutorial does not repeat it.

## The station

One device, unit 1, "Pump station":

| Table | Address | Type | Meaning |
|---|---|---|---|
| holding | 0-1 | float32 | level setpoint (m), default 2.5, writable |
| holding | 2 | u16 | mode: 0 off, 1 auto, 2 manual (default 1) |
| holding | 10-17 | string, 8 registers | site name, `NORTH-01`, read-only |
| coil | 0 | bit | pump run command (manual mode) |
| discrete | 0 | bit | high-level alarm |
| discrete | 1 | bit | low-level alarm |
| discrete | 2 | bit | pump running feedback |
| input | 0-1 | float32 | tank level (m) |
| input | 2-3 | float32 | outflow (m3/h) |
| input | 4 | u16 | motor current, 0.1 A units |
| input | 10-11 | u32 | pump run seconds |

The example adds an inlet flow meter at input 6-7 (float32, low word first, the way some old meters
send it), an event log (file 1) and an alarm history (FIFO queue 100).

## Install

The package is pure `ctypes` over `libmogbus.so`: no compiled extension, Python 3.9 or newer, Linux
only (on Windows use [WSL2](../windows/windows-wsl2.md)). From a checkout:

```bash
make python              # builds libmogbus.so and copies it into python/mogbus/
pip install ./python     # the wheel contains the library
```

or point the package at any library build without installing it:

```bash
make lib
MOGBUS_LIB=$PWD/libmogbus.so PYTHONPATH=python python3 -c "import mogbus; print(mogbus.library_version())"
```

```text
0.0.1
```

\ref core.py "The API overview" lists where the package looks for the library and the limits it
inherits from the C library (16 devices and 8 servers per context).

## Step 1: build the device

A \ref mogbus::core::Context "Context" owns devices and servers; use it as a context manager so
everything is released on exit. \ref mogbus::core::Context::add_device "Context.add_device()" gives
a device, and \ref mogbus::core::Device::add_point "Device.add_point()" adds points to its four
tables:

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

Things worth noticing:

- Spaces, types and orders are plain strings (`"holding"`, `"f32"`, `"low-first"`); the enum members
  (\ref mogbus::core::Space "Space", \ref mogbus::core::ValueType "ValueType") work as well, and a
  wrong name raises \ref mogbus::core::MogbusError "MogbusError" listing the valid ones.
- A point owns as many addresses as its type needs: the float32 setpoint is addresses 0 and 1.
  Only configured points exist; an address with no owner stays undefined and a client gets exception
  02 for it.
- `read_only=True` makes every Modbus write that touches the site name fail with exception 02, while
  the host can still change it with
  \ref mogbus::core::Device::set_string "Device.set_string()".
- The inlet flow is a *ramp*: it moves by itself (20, 24, 28 ... 60, then back), so a client sees a
  live-looking value without any code of yours running. `byte_order="little"` would swap the bytes
  of every register as well; with `word_order` that gives the four classic layouts.
- Points are created with a start value; `u64` and `s64` start values are floats here, so integers
  above 2^53 round (\ref mogbus::core::Device::read "Device.read()" is always exact).

Check what you built with \ref mogbus::core::Device::info "Device.info()":

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

```text
'Pump station', unit 1, points per table: {'coil': 1, 'discrete': 3, 'input': 5, 'holding': 3}
site name from the host: NORTH-01
rejected: address is already used by another point
```

## Step 2: serve it and read it

\ref mogbus::core::ServerOptions "ServerOptions" holds the settings of a transport; the defaults are
those of a config file (TCP on `127.0.0.1:1502`, loopback only). The example picks a free port and
turns on the HTTP endpoint that [step 7](python-observe.md) uses. \ref mogbus::core::Context::check_server "Context.check_server()"
validates the options without starting anything, \ref mogbus::core::Context::serve "Context.serve()"
creates the server and \ref mogbus::core::Server::start "Server.start()" runs it on a background
thread, so your test keeps control of the process:

```python
options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1", http_port=free_port(),
                               http_control=True)
ctx.check_server(options)
server = ctx.serve(options).start()
print("alive:", mogbus.probe(options, 1, 1000))
```

\ref mogbus::core::probe "probe()" is the client side of `mogbus --health`: one echo request to a
unit, true when the server answers. It is how a test waits for a server to be ready. The example's
client is [`modbus_tcp.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/modbus_tcp.py),
about a hundred lines of `socket` and `struct`: it stands in for your own client code (pymodbus
works the same way against this server).

```python
with Client(options.port) as plc:
    print("site:", plc.read_string(SITE_NAME, 8))
    print("setpoint:", plc.read_f32(3, SETPOINT), "mode:", plc.holding(MODE)[0])
    print("level registers:", plc.input(LEVEL, 2), "-> level", plc.read_f32(4, LEVEL))
    try:
        plc.write_registers(SITE_NAME, [0x4F4B])
    except ModbusException as exc:
        print("write to the read-only name:", exc)
```

```text
alive: True
site: NORTH-01
setpoint: 2.5 mode: 1
level registers: [16384, 0] -> level 2.0
write to the read-only name: function 0x10 failed with exception 02
```

`[16384, 0]` is 2.0 as an IEEE float32 (`0x4000 0x0000`), high word first as the default
`word_order` says. The ramp is read with the words swapped, as that legacy meter sends them:

```python
first = plc.read_f32(4, INFLOW, low_first=True)
time.sleep(1.2)
second = plc.read_f32(4, INFLOW, low_first=True)
print(f"inlet flow moved by itself: {first:.0f} -> {second:.0f} m3/h")
```

```text
inlet flow moved by itself: 20 -> 24 m3/h
```

The exact numbers depend on when you start; they climb by 4 every second. The simulation of the next
page needs a repeatable inflow, so the example pins the ramp with
\ref mogbus::core::Device::set_value "Device.set_value()", which takes the point over from the clock
(it is "host-owned" from now on):

```python
dev.set_value("input", INFLOW, 40.0)
```

Next: [Driving the process from the host](python-process.md).

## See also

- \ref core.py "Python API reference"
- [Testing your client](python-testing.md)
- [Windows route](../windows/windows-route.md)
