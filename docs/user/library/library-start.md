# Embedding mogbus in C

Link the mogbus library into your own program: create devices, serve them over any transport from your own loop, move their values from the host side and veto or react to client requests.

The `mogbus` binary is one front end. The same code is a static and a shared library with a small C API, so a program can be a Modbus test harness. The API itself is described in the @ref mogbus.h "C API reference". The [pumping station tutorial](../tutorial/tutorial-start.md) builds one realistic harness step by step, with the C code next to the config and the Python version; the pages below are the reference for what it uses.

From Python, see [Using mogbus from Python](../python/python-start.md): a `ctypes` package over `libmogbus.so`.

## Pages

- [Devices and points](library-device.md): context, points of every type, strings, ramps, file records, FIFOs, devices from a config file.
- [Servers](library-serve.md): options, several servers, your own loop or event loop, probing, the HTTP endpoint.
- [Host writes](library-simulate.md): client writes against host writes, decoded and raw values, strings.
- [Hooks](library-hooks.md): the access hook, the post-write hook, vendor function codes, frame and connection hooks.
- [Logging, shutdown, lifetimes and threading](library-lifetimes.md)
- [The C client](library-client.md)

## Build and link

You need two things: the header [`include/mogbus.h`](https://github.com/BolvicBolvicovic/mogbus/blob/main/include/mogbus.h) (the whole API; it needs only `<stdint.h>`, `<stdbool.h>` and `<stddef.h>`) and the library, either static (`libmogbus.a`) or shared (`libmogbus.so`). Both libraries contain the whole simulator, including the config parser, so nothing else of mogbus is needed. The shared library exports only `mogbus_*` symbols.

The release tarball ships them as `lib/libmogbus.a`, `lib/libmogbus.so` and `include/mogbus.h`. From a source checkout, build them with

```bash
make -j$(nproc) lib          # libmogbus.a and libmogbus.so in the repository root
```

Then compile your program against the header and link the library you chose. Assuming the tarball layout (`include/` and `lib/` next to each other; in a checkout use `-Iinclude` and `-L.`):

```bash
# static: the library is part of your executable; add what it needs (-lm, and OpenSSL when the library was built with TLS)
cc -Iinclude pump_station.c lib/libmogbus.a -lm -lssl -lcrypto -o pump_station

# shared: the library is found when the program starts, here next to it through an rpath
cc -Iinclude pump_station.c -Llib -lmogbus -Wl,-rpath,'$ORIGIN/lib' -o pump_station
```

With `pkg-config` the flags come from `mogbus.pc`, which [`make install`](../building.md#install) writes (the release tarball has it in `lib/pkgconfig/`; point `PKG_CONFIG_PATH` at that directory, or install into a prefix pkg-config already searches):

```bash
cc $(pkg-config --cflags mogbus) pump_station.c $(pkg-config --libs mogbus) -o pump_station            # shared
cc $(pkg-config --cflags mogbus) pump_station.c $(pkg-config --static --libs mogbus) -o pump_station   # --static also adds -lm and OpenSSL
```

A library built without TLS (`make WITH_TLS=0`) needs neither `-lssl` nor `-lcrypto`, and creating a server with `tls_enabled` then fails with an error. A library built without the config loader (`make WITH_CONFIG=0`) has the same header and symbols, but @ref mogbus_load_config "mogbus_load_config()" returns `MOGBUS_ERR_UNSUPPORTED` and @ref mogbus_server_create_from_config "mogbus_server_create_from_config()" returns NULL, both with `built without config support` in `mogbus_last_error()`. See [Build from source](../building.md) for all build options.

With a shared library, `LD_LIBRARY_PATH` works instead of the rpath. The version is available at run time from @ref mogbus_version "mogbus_version()" and at compile time from `MOGBUS_VERSION_MAJOR`, `MOGBUS_VERSION_MINOR` and `MOGBUS_VERSION_PATCH`. The tutorial program is POSIX (it uses `fork` and `poll`); the library API is the same in the [native Windows build](../windows/windows-native.md).

## Run it

Build the tutorial program ([the tutorial](../tutorial/tutorial-start.md)) with one of the commands above (from a checkout, with `examples/tutorial/pump_station.c` as the source) and run it for a few seconds:

```bash
./pump_station --seconds 7 --port 1502
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

Point your client at `127.0.0.1:1502`, unit 1, while it runs.

The options belong to the tutorial program, not to mogbus; they are parsed by its own `main()`:

- `--port N` sets the Modbus TCP port and `--seconds N` stops the program after N seconds (the default is to run until Ctrl-C).
- `--speed X` (default 10) scales the simulated time.
- `--self-test` makes the program its own Modbus client: it serves the station, sends a series of requests to itself, prints one `ok` or `FAIL` line for each check and exits 0 when every check passed. The tutorial quotes those lines to show what each API call does on the wire.
- `--config FILE` loads a config file first (see [devices and points](library-device.md#building-the-device-from-a-config-file)), `--unit N` moves the station to another unit id so it can coexist with the devices of that file, `--http PORT` turns on the web UI and `--verbose` logs every request.

## See also

- @ref mogbus.h "C API reference"
- [Using mogbus from Python](../python/python-start.md)
- [Pumping station tutorial](../tutorial/tutorial-start.md)
- [Config reference](../config/format.md)
