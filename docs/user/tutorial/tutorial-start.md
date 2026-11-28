# Tutorial: simulate a pumping station

> **Summary:** Build one simulated water pumping station and drive it three ways, from a config file and the `mogbus` binary, from C and from Python, in the same eight steps, to test a SCADA screen or a data logger without the real PLC.

A team is writing a SCADA screen and a data logger for a water pumping station. The real PLC is not on
their desk (or they do not want a test to open its valves), and they want to test their code against
something that behaves like it, including alarms, refused writes, slow answers and an event log. This
tutorial builds that something. About an hour for one route.

The station has one tank and one pump. The tank fills at a steady rate and the pump drains it. In auto
mode the pump starts when the level is a little above the setpoint and stops a little below it; in
manual mode a client switches it with a coil.

## Three routes through the same steps

Every step shows its code as tabs, and the choice is remembered from step to step:

- **Scheme and Bash**: a config file and the `mogbus` binary. No programming; it covers machines, faults and the HTTP endpoint, which the C and Python APIs cannot build.
- **C**: your own program owns the devices and the process behind them and steps the server from its own loop (the library API, [Embedding mogbus in C](../library/library-start.md)).
- **Python**: the same in Python, with `ctypes` over the library ([Using mogbus from Python](../python/python-start.md)).

When a step only makes sense for some of the routes, only those tabs are shown and the text says so.
Where a route does something its own way (the config describes the process with a machine, C and
Python drive it from the host), the text around the blocks says which is which.

| Step | What you do |
|---|---|
| [1. Build the device](tutorial-device.md) | points of every kind, strings, ramps, file records, a FIFO |
| [2. Serve it and read it](tutorial-serve.md) | validate, pick a transport, serve, the first client |
| [3. Make it behave](tutorial-simulate.md) | the tank fills and the pump runs: machines, or the host side of the API |
| [4. Guard clients and keep records](tutorial-guard.md) | refused writes, an event log, an alarm FIFO, a vendor function code |
| [5. Break it on purpose](tutorial-faults.md) | delays, scheduled sensor failures, runtime control |
| [6. Observe it](tutorial-observe.md) | web UI, health, metrics, logs |
| [7. Test your client](tutorial-test.md) | the station as a test fixture |
| [8. Stop it and ship it](tutorial-ship.md) | signals, shutdown, threads, a container |

## The station

One device, unit id 1, "Pump station":

| Table | Address | Type | Meaning |
|---|---|---|---|
| holding | 0-1 | float32 | level setpoint (m), default 2.5, writable |
| holding | 2 | u16 | mode: 0 off, 1 auto, 2 manual (default 1) |
| holding | 10-17 | string, 8 registers | site name, `NORTH-01`, read-only |
| coil | 0 | bit | pump run command (manual mode) |
| discrete | 0 | bit | high-level alarm |
| discrete | 1 | bit | low-level alarm |
| discrete | 2 | bit | pump running |
| input | 0-1 | float32 | tank level (m) |
| input | 2-3 | float32 | outflow (m3/h) |
| input | 4 | u16 | motor current, 0.1 A |
| input | 10-11 | u32 | pump run seconds |

Each route adds a few points of its own to show more of the API: the config has a wall clock at input
20-21 (u32, unix seconds); the C program has an inlet temperature ramp (input 6), a wall-clock minute
(input 20), an event log in file 1 and a level history in a FIFO at address 0; the Python program has
an inlet flow meter at input 6-7 (float32, low word first, the way some old meters send it), an event
log (file 1) and an alarm history (FIFO queue 100).

## The files

All three are checked in CI, so the code in these pages cannot rot:

| Route | File | Checked by |
|---|---|---|
| Config | [`examples/tutorial/pump-station.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/tutorial/pump-station.scm) | `mogbus --check` |
| C | [`examples/tutorial/pump_station.c`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/tutorial/pump_station.c) | `make lib-check` (its `--self-test`) |
| Python | [`examples/python/pump_station.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/pump_station.py) | `make python-examples` |

The code shown in a step is quoted from them, and the output is what they print. The Python program
also uses a small Modbus TCP client,
[`modbus_tcp.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/modbus_tcp.py)
(about a hundred lines of `socket` and `struct`), that stands in for your own client code; pymodbus
works the same way against this server. The Config and C routes use
[`tools/modbus_probe.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/tools/modbus_probe.py)
(no dependencies) or any other client for the same job.

## Run it

You need a built `./mogbus` ([Get started](../get-started.md), step 1). Run every command from the
repository root.

Config: start the server and leave it running.

```bash
./mogbus examples/tutorial/pump-station.scm
```

C: build the program against the library and run it for seven seconds (a static link needs `-lssl
-lcrypto` only when the library was built with TLS).

```bash
make -j$(nproc) lib
cc -Iinclude examples/tutorial/pump_station.c libmogbus.a -lm -lssl -lcrypto -o pump_station
./pump_station --seconds 7 --port 1502
```

Python: build the package and run the program, which runs every step in order, prints what the steps
show and exits with status 0 when all its checks pass (each prints an `ok` line).

```bash
make python
PYTHONPATH=python python3 examples/python/pump_station.py
```

Build options for the C program are in [Embedding mogbus in C](../library/library-start.md#build-and-link),
the Python install notes in [Using mogbus from Python](../python/python-start.md#install).

## See also

- [Embedding mogbus in C](../library/library-start.md) and [Using mogbus from Python](../python/python-start.md): build, install and reference pages.
- [Config reference](../config/format.md)
- [Profile catalog](../profiles/profile-catalog.md): ready-made devices to start from.
