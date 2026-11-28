# Using mogbus from Python

Build a simulated water pumping station in Python, serve it over Modbus TCP and test your own client code against it.

You maintain a SCADA screen, a data logger or some control code that talks Modbus to a pumping
station, and the real PLC is not on your desk (or you do not want a test to open its valves). In this
tutorial you build the station with the `mogbus` Python package, serve it from a background thread,
make its process behave (the tank fills, the pump runs, alarms rise), enforce the rules of the real
controller, watch what your client does and finally turn it into automated tests. The complete
program is
[`examples/python/pump_station.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/pump_station.py);
the output shown in the steps is what it prints, and the code shown is quoted from it.

This is the Python counterpart of [Embedding mogbus in C](../library/library-start.md), with the same
five steps in the same order:

| Step | Page | What you do |
|---|---|---|
| 1 | [Build the device](python-device.md) | points of every kind, strings, ramps, file records, a FIFO, or a device from a config file |
| 2 | [Serve it and read it](python-serve.md) | server options and transports, `serve`, probing, the first client |
| 3 | [Simulate the process from the host](python-simulate.md) | tank physics, host writes against client writes |
| 4 | [Guard clients and keep records](python-hooks.md) | the access hook, post-write hook, event log, alarm FIFO, a vendor function code, frame hook, connection hook |
| 5 | [Observe, shut down, lifetimes and threading](python-lifetimes.md) | logs, metrics, runtime control, the process on a thread, shutdown |
| 6 | [Test your client](python-testing.md) | unittest and pytest fixtures |

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

The program names these addresses as constants at the top of the file, so the code says `SETPOINT`
instead of `0`: `SETPOINT`, `MODE` and `SITE_NAME` for the holding registers, `RUN_COMMAND` for the
coil, `HIGH_ALARM`, `LOW_ALARM` and `RUNNING` for the discrete inputs, `LEVEL`, `OUTFLOW`,
`CURRENT`, `INFLOW` and `RUN_SECONDS` for the input registers, and `EVENT_FILE` and `ALARM_FIFO`
for the two stores. `OFF`, `AUTO` and `MANUAL` are the values of the mode register.

The program also contains a small Modbus TCP client,
[`modbus_tcp.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/modbus_tcp.py),
about a hundred lines of `socket` and `struct`. It stands in for your own client code (pymodbus works
the same way against this server) and appears in the examples as `Client`, used as `plc`.

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

## Run it

From a checkout, with the package built as above:

```bash
PYTHONPATH=python python3 examples/python/pump_station.py
```

The program runs every step in order, prints what the steps below show and exits with status 0 when
all its checks pass (each prints an `ok` line).

## See also

- \ref core.py "Python API reference"
- [Embedding mogbus in C](../library/library-start.md)
- [Windows route](../windows/windows-route.md)
