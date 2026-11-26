# Using mogbus from Python

The `mogbus` Python package starts simulated devices from a test or a script, lets you change their values while a client talks to them, and reads what the client did.

It wraps [`libmogbus.so`](../library/library-start.md) with `ctypes`: no compiled extension, standard
library only, Python 3.9 or newer, Linux only (on Windows it works inside
[WSL2](../windows/windows-wsl2.md)). It mirrors the C API in `include/mogbus.h`, so the limits are the
same: 16 devices and 8 servers per context, machines and faults only from config files.

## Install

From a checkout:

```bash
make python              # builds libmogbus.so and copies it into python/mogbus/
pip install ./python     # the wheel contains the library
```

Or use any library build without installing it into the package:

```bash
make lib
MOGBUS_LIB=$PWD/libmogbus.so PYTHONPATH=python python3 -c "import mogbus; print(mogbus.library_version())"
```

The library is looked up in this order:

1. the `MOGBUS_LIB` path (an error if it fails to load, no fallback);
2. a `libmogbus.so` inside the package;
3. `libmogbus.so` at the checkout root (when running from `python/`);
4. `ctypes.util.find_library("mogbus")`.

The import fails with a message that lists what was tried, or when the library's major.minor version
differs from the package's. `mogbus.library_version()` returns `mogbus_version()`.

## Quick start

```python
import socket, struct, mogbus

config = """
(device (name "meter") (unit-id 1)
  (holding 0 (static 42)))
"""
with mogbus.serve(config_text=config, port=0) as srv:   # TCP on a free loopback port
    meter = srv.device(1)
    meter.set_value("holding", 0, 100)                  # the client now reads 100
    with socket.create_connection(("127.0.0.1", srv.port)) as s:
        s.sendall(struct.pack(">HHHBBHH", 1, 0, 6, 1, 3, 0, 1))
        print(struct.unpack(">H", s.recv(256)[9:11]))   # (100,)
```

The same with a profile file:

```python
import mogbus

with mogbus.serve(config="profiles/minimal.scm", port=0) as srv:
    print("Modbus TCP on port", srv.port)
    srv.device(1).set_value("holding", 0, 1234)   # a client now reads 1234
```

`serve(config=path, config_text=text, **server_options)` creates a context, loads the config, serves
it from a background thread and yields an object with `.context`, `.server`, `.port` (the port
actually served) and `.device(unit_id)`. On exit it stops the thread and destroys the context.

## Config and overrides

With a config the server uses exactly what the binary would (the config's `(server ...)` section, or
the defaults, TCP on 1502, when it has none). Only the keyword arguments you pass override its fields:

```python
mogbus.serve(config=path, port=0, mode="rtu-tcp", idle_timeout_ms=500)
```

`port=0` in a network mode picks a free loopback port; without a config you get TCP on a free port.

## Limits

Linux only. Same limits as the C API: 16 devices and 8 servers per context, servers serve the devices
that exist when they are created, machines and faults only from config files, and a TLS server needs
a library built with TLS (the host process should ignore `SIGPIPE`). The package installs no signal
handlers. The wheel contains a native library, so build and install it on the machine (or an equal
one) that runs the tests.

## See also

- [Python API reference](python-api.md)
- [Threading and logging](python-threading.md)
- [Testing with pytest](python-testing.md)
