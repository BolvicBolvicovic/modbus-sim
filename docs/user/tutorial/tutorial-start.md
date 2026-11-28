# Tutorial: simulate a pumping station

> **Summary:** Build one simulated water pumping station and drive it three ways, from a config file and the `mogbus` binary, from C and from Python, in the same eight steps, to test a SCADA screen or a data logger without the real PLC.

A team is writing a SCADA screen and a data logger for a water pumping station. The real PLC is not on
their desk (or they do not want a test to open its valves), and they want to test their code against
something that behaves like it, including alarms, refused writes, slow answers and an event log. This
tutorial builds that something. It takes about an hour for one route.

You need to know Modbus and your own equipment. You do not need to know mogbus, C or Python well: every
code block is a complete file that you can copy, build and run, and the comments in it say what each line
means in Modbus terms.

The station has one tank and one pump. The tank fills at a steady rate and the pump drains it. In auto
mode the pump starts when the level is a little above the setpoint and stops a little below it; in
manual mode a client switches it with a coil.

## Three routes through the same steps

Every step shows its code as tabs, and the choice is remembered from step to step:

- **Scheme / Bash**: a config file and the `mogbus` program. No programming. A config can also describe things that need no code at all: values that move by themselves, state machines, faults.
- **C**: your own program holds the devices and runs the server ([Embedding mogbus in C](../library/library-start.md)).
- **Python**: the same in Python, with the `mogbus` package ([Using mogbus from Python](../python/python-start.md)).

All three routes end up with the same station, on the same Modbus address, and you can mix them: a C or
Python program can load a config file with `mogbus_load_config()` / `Context.load_config()` and still use
its host side (hooks, `set_value`). Where a feature exists only in a config file (machines, faults), the
page says so in one sentence and shows how the other routes get it.

## Before you start

Build the program and the library once, from the repository root ([Get started](../get-started.md)):

```bash
make -j$(nproc)           # the mogbus program, ./mogbus
make -j$(nproc) lib       # the library: libmogbus.a and libmogbus.so
```

Run every command of the tutorial from that directory and save the files you create there.

**Scheme / Bash**: nothing else. Save a config as `station.scm` and run `./mogbus station.scm`.

**C**: save a program as `name.c`, build it and run it. `-lssl -lcrypto` are needed only when the library
was built with TLS.

```bash
cc -Iinclude name.c libmogbus.a -lm -lssl -lcrypto -o name
./name
```

**Python**: save a script as `name.py` and run it with the package on the path.

```bash
PYTHONPATH=python python3 name.py
```

The programs print the log lines of the library on the error output, such as `listening on
127.0.0.1:1502`. The examples do not show them again.

### Handling errors

Every program uses one error pattern, so you can ignore it after reading this once.

- **C**: a function that can fail returns a `mogbus_result_t`. `MOGBUS_OK` means success. Anything else is a failure: the program prints the text from `mogbus_last_error(ctx)` and stops with `return 1`. Functions that open something take the address of a pointer as their last parameter and set it to NULL on failure. Where a program has no context yet, the text comes from `mogbus_last_error(NULL)`. A client has its own message, `mogbus_client_last_error(client)`.
- **Python**: a failure raises `mogbus.MogbusError`, and the script stops with its message. A Modbus client call that the server answers with an exception raises `mogbus.ModbusException`, a kind of `MogbusError` whose `code` is the Modbus exception code.

The programs do not release memory when they stop on an error: the operating system does that.

## The station

One device, Modbus unit id 1, named "Pump station". Addresses are protocol addresses, counted from 0
(register 40001 of a PLC manual is address 0 of the holding table).

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
| input | 6-7 | float32 | inlet temperature (degrees C), moves by itself |
| input | 10-11 | u32 | pump run seconds |
| input | 20-21 | u32 | wall clock, unix seconds |
| file 1 | records 0-1 | u16 | event log |
| FIFO | 0 | u16, up to 31 | alarm history |

## The steps

| Step | What you do |
|---|---|
| [1. Build the device](tutorial-device.md) | the points above, in a config and in code |
| [2. Serve it and read it](tutorial-serve.md) | start the server, read it with a client |
| [3. Make it behave](tutorial-simulate.md) | the tank fills and the pump runs |
| [4. Guard clients and keep records](tutorial-guard.md) | refused writes, an event log, a FIFO, a vendor function code |
| [5. Break it on purpose](tutorial-faults.md) | slow and failing answers, set from a test |
| [6. Observe it](tutorial-observe.md) | web UI, health, metrics, logs |
| [7. Test your client](tutorial-test.md) | the station as a test fixture |
| [8. Stop it and ship it](tutorial-ship.md) | signals, threads, a container |

## Run the finished station

The finished config, [`examples/tutorial/pump-station.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/tutorial/pump-station.scm),
is steps 1, 3, 5 and 6 together. If you want to see where you are going, run it:

```bash
./mogbus examples/tutorial/pump-station.scm
```

The repository also holds two larger programs that put all the steps of the C and Python routes into
one file each, with checks that run in CI: [`examples/tutorial/pump_station.c`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/tutorial/pump_station.c)
and [`examples/python/pump_station.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/pump_station.py).
The tutorial pages do not quote them: read them after the tutorial, when you want to see how the pieces
fit in a real program.

## See also

- [Embedding mogbus in C](../library/library-start.md) and [Using mogbus from Python](../python/python-start.md): build, install and reference pages.
- [Config reference](../config/format.md)
- [Profile catalog](../profiles/profile-catalog.md): ready-made devices to start from.
