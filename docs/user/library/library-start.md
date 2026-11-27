# Embedding mogbus in C

Build a Modbus test harness around mogbus: a program that owns a simulated water pumping station, runs the process behind it, and serves it to the client you want to test.

The `mogbus` binary is one front end. The same code is a static and a shared library with a small C API, so your own program can create devices, serve them over any transport from its own loop, move their values from the host side and veto or react to client requests. This tutorial walks through one realistic program, step by step. The API itself is described in the @ref mogbus_t "C API reference"; the tutorial links to it and does not repeat signatures.

From Python, see [Using mogbus from Python](../python/python-start.md): a `ctypes` package over `libmogbus.so`.

## The job

A team is writing a SCADA screen and a data logger for a water pumping station. The real PLC is not available, and they want to test their code against something that behaves like it, including alarms, refused writes and an event log. The harness serves unit 1, "Pump station", with these points:

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

The program adds an inlet temperature ramp (input 6), a wall-clock minute (input 20), an event log in file 1 and a level history in a FIFO at address 0. The same station is built from a config file in [the app tutorial](../tutorial-app.md) and from Python in [the Python tutorial](../python/python-start.md).

## What the tutorial covers

The complete program is
[`examples/tutorial/pump_station.c`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/tutorial/pump_station.c).
It runs in CI through `make lib-check`, so it cannot rot. Each step explains one part of it.

1. [Build the device](library-device.md): context, device, points of every kind, strings, read-only points, ramps, clock fields, file records, FIFO and loading a config.
2. [Serve it from your own loop](library-serve.md): server options and modes, `mogbus_server_step`, probing, errors, the HTTP endpoint and web UI.
3. [Simulate the process from the host](library-simulate.md): client writes against host writes, decoded and raw values, strings.
4. [Guard clients and keep records](library-hooks.md): the access hook, file records as an event log, a FIFO refreshed on demand.
5. [Observe, shut down, lifetimes and threading](library-lifetimes.md): logging, metrics, signals, ownership rules.

## Build and link

```bash
make -j$(nproc) lib          # libmogbus.a + libmogbus.so
make -j$(nproc) lib-check    # also builds and runs the examples against both
```

`make` alone builds only the `mogbus` binary, which is itself a client of this API linked against the static library. `make WITH_CONFIG=0` builds the libraries instead, since there is no binary then. The `.a` contains the whole simulator, including the vendored s7 parser, so it is self-contained. The `.so` exports only `mogbus_*` symbols; everything else is hidden. The only header is [`include/mogbus.h`](https://github.com/BolvicBolvicovic/mogbus/blob/main/include/mogbus.h); it needs just `<stdint.h>`, `<stdbool.h>` and `<stddef.h>`.

```bash
# shared (found at run time via rpath or LD_LIBRARY_PATH)
cc -Iinclude examples/tutorial/pump_station.c -L. -lmogbus -Wl,-rpath,'$ORIGIN' -o pump_station
# static: also link what the library needs (-lm, plus -lssl -lcrypto in a TLS build)
cc -Iinclude examples/tutorial/pump_station.c libmogbus.a -lm -lssl -lcrypto -o pump_station
```

Build options:

- `WITH_TLS=auto|0|1` (default `auto`): OpenSSL support, see [Build from source](../building.md). A `WITH_TLS=0` build has no OpenSSL dependency; creating a server with `tls_enabled` then fails with an error.
- `WITH_CONFIG=1|0` (default `1`): `0` leaves out the config file loader and the vendored s7, so the libraries serve only devices built in code. The header and the exported symbols are identical, so the ABI does not change: @ref mogbus_load_config "mogbus_load_config()" returns false and @ref mogbus_server_create_from_config "mogbus_server_create_from_config()" returns NULL, both with the error `built without config support`. Combine freely with `WITH_TLS=0`.

The current version is 0.0.1 (`MOGBUS_VERSION_*`, @ref mogbus_version "mogbus_version()"). The tutorial program is POSIX (it uses `fork` and `poll`); the library API is the same in the [native Windows build](../windows/windows-native.md).

## Run it

```bash
./examples/tutorial/pump_station_static --seconds 7 --port 1502
```

```text
mogbus 0.0.1 pump station
device "Pump station", unit 1: 3 holding, 6 input, 1 coil, 3 discrete points
serving 1 device(s) on 127.0.0.1:1502
self-check: mogbus_probe answered
press Ctrl-C to stop
level 2.55 m, pump ON, events 3, client writes 0 ok / 0 refused
level 2.55 m, pump off, events 6, client writes 0 ok / 0 refused
level 2.25 m, pump ON, events 9, client writes 0 ok / 0 refused
stopping
```

Point your client at `127.0.0.1:1502`, unit 1, while it runs. `--speed X` (default 10) scales the simulated time. `--self-test` makes the program its own client and exits 0 when every check passes, which is how CI runs it; `--config FILE` loads a profile first, `--unit N` moves the station to another unit id so the two can coexist, and `--http PORT` turns on the web UI. The checks of the self-test are quoted in the steps below.

## See also

- @ref mogbus_t "C API reference"
- [Using mogbus from Python](../python/python-start.md)
- [Config reference](../config/format.md)
