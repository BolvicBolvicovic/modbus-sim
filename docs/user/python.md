# Using mogbus from Python

The `mogbus` Python package wraps [`libmogbus.so`](library.md) with `ctypes`: no compiled
extension, standard library only, Python 3.9 or newer, Linux only. It mirrors the C API in
`include/mogbus.h`, so the limits are the same (16 devices and 8 servers per context, machines and
faults only from config files). Use it to start simulated devices from a test, change their
values while a client talks to them, and read what the client did.

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

The library is looked up in this order: the `MOGBUS_LIB` path (an error if it fails to load, no
fallback), a `libmogbus.so` inside the package, `libmogbus.so` at the checkout root (when running
from `python/`), then `ctypes.util.find_library("mogbus")`. The import fails with a message that
lists what was tried, or when the library's major.minor version differs from the package's.
`mogbus.library_version()` returns `mogbus_version()`.

## Quick start

```python
import socket, struct, mogbus

config = """
(device (name "meter") (unit-id 1)
  (holding 0 (static 42)))
"""
with mogbus.serve(config_text=config) as srv:           # TCP on a free loopback port
    meter = srv.device(1)
    meter.set_value("holding", 0, 100)                  # the client now reads 100
    with socket.create_connection(("127.0.0.1", srv.port)) as s:
        s.sendall(struct.pack(">HHHBBHH", 1, 0, 6, 1, 3, 0, 1))
        print(struct.unpack(">H", s.recv(256)[9:11]))   # (100,)
```

`serve(config=path, config_text=text, mode="tcp", port=0, **server_options)` creates a context,
loads the config, picks a free port when `port` is 0 (network modes), starts the server on a
background thread and yields an object with `.context`, `.server`, `.port` and `.device(unit_id)`.
On exit it stops the thread and destroys the context. The `(server ...)` section of the config is
ignored; pass transport settings as keyword arguments (`mode="rtu-tcp"`, `idle_timeout_ms=...`).

## API overview

| Python | C |
|---|---|
| `Context()`, `.close()`, `with` | `mogbus_create`, `mogbus_destroy` |
| `ctx.load_config(path)` | `mogbus_load_config` |
| `ctx.add_device(unit_id, name=None)`, `ctx.find_device(unit_id)`, `ctx.devices`, `iter(ctx)`, `len(ctx)` | `mogbus_add_device`, `mogbus_find_device`, `mogbus_device_at/count` |
| `dev.add_point(space, address, type="u16", value=0, word_order="high-first", ramp=None)` | `mogbus_device_add_point` |
| `dev.read(space, address, count)`, `dev.write(space, address, values)` | `mogbus_device_read/write` (raw registers) |
| `dev.get_value(space, address)`, `dev.set_value(space, address, value)` | `mogbus_device_get_value/set_value` (decoded) |
| `dev.info()` | `mogbus_device_info` |
| `ctx.serve(options=None, **overrides)`, `ctx.serve_config()` | `mogbus_server_create`, `mogbus_server_create_from_config` |
| `ctx.config_server_options()`, `ctx.check_server(...)` | `mogbus_config_server_options`, `mogbus_server_check` |
| `server.step(max_wait_ms)`, `.run()`, `.start()`, `.stop()`, `.close()` | `mogbus_server_step/run/stop/close` |
| `server.pty_name`, `server.metrics()` | `mogbus_server_pty_name`, `mogbus_server_metrics` |
| `probe(options, unit_id, timeout_ms)`, `probe_or_raise(...)`, `rtu_timing(baud, frame_gap_ms)` | `mogbus_probe`, `mogbus_rtu_timing` |
| `set_log_level`, `set_log_format`, `set_log_sink` | `mogbus_set_log_level/format/sink` |

Spaces (`"coil"`, `"discrete"`, `"input"`, `"holding"`), types (`"u16"`, `"s16"`, `"u32"`, `"s32"`,
`"f32"`, `"bool"`), word orders (`"high-first"`, `"low-first"`), ramp modes (`"wrap"`, `"bounce"`,
`"hold"`), server modes (`"tcp"`, `"rtu-tcp"`, `"ascii-udp"`, ...) and the other enums are accepted
as strings or as the `mogbus.Space`, `mogbus.ValueType`, ... enum members (also plain ints).
Anything else raises `MogbusError` listing the valid names.

- A ramp is a dict or `mogbus.Ramp`: `{"from": 0, "to": 100, "step": 10, "every_ms": 1000, "mode": "bounce"}`.
- `ServerOptions` has the fields of `mogbus_server_options_t` in snake_case with the defaults of
  `mogbus_server_options_defaults()` (TCP on `127.0.0.1:1502`). Strings are encoded and kept
  alive for the duration of the C call; the library copies them.
- `dev.info()` returns a `DeviceInfo` with `name`, `unit_id`, `point_counts` per space, `faults`
  (only configured kinds, e.g. `"delay"`) and `custom_function_codes`.
- Errors raise `MogbusError` with the message of the C `err` buffer. A failed server creation
  also sets `error.status` (`mogbus.Status.INVALID`, `SERIAL`, `LISTEN`).
- `dev.write()` is what a client write does (it fails on a point claimed by `set_value()`);
  `dev.set_value()` is the host side and makes the point host-owned. See [library.md](library.md).
- `probe()` returns a bool; `probe_or_raise()` raises with the reason. Both block the caller and
  take no lock, so they work while a started server runs on its background thread.

## Threading

The C library is not thread-safe, so the package serialises it: every `Context`, `Device` and
`Server` method takes one re-entrant lock per context.

- `server.start()` runs a background thread that calls `step(10 ms)` under that lock and releases
  it between steps. While it runs you may read and write device values from any Python thread;
  a call waits for the current step (at most about 10 ms when the server is idle).
- `server.run()` is the same loop on the calling thread and blocks until `stop()`.
  `run(exclusive=True)` calls `mogbus_server_run()` and holds the lock for the whole run.
- `stop()` takes no lock (it is `mogbus_server_stop()`) and joins the thread. `close()` and
  `Context.close()` stop and join every thread first, then free the C memory; closing twice is
  safe. After `Context.close()` its devices and servers raise `MogbusError("context is closed")`.
- Logging is process-wide. `set_log_sink(callable)` calls `callable(level, message)` on the
  thread that logs (the server thread, with the context lock held), so keep it short and do not
  block; `None` restores stderr. `set_log_format("json")` makes `message` a JSON object.

## pytest

pytest is not required; a fixture is a few lines:

```python
import mogbus, pytest

@pytest.fixture
def meter():
    with mogbus.serve(config="profiles/power-meter.scm") as srv:
        yield srv

def test_voltage(meter):
    meter.device(1).set_value("input", 0, 230.0)
    # connect your client to 127.0.0.1:meter.port
```

## Limits

Linux only. Same limits as the C API: 16 devices and 8 servers per context, servers serve the
devices that exist when they are created, machines and faults only from config files, and a TLS
server needs a library built with TLS (the host process should ignore `SIGPIPE`). The package
installs no signal handlers. The wheel contains a native library, so build and install it on the
machine (or an equal one) that runs the tests.
