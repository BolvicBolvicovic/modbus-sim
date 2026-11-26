# Python API reference

The Python calls and the C functions they wrap, the accepted names for spaces and types, and how errors and values behave.

## Calls

| Python | C |
|---|---|
| `Context()`, `.close()`, `with` | `mogbus_create`, `mogbus_destroy` |
| `ctx.load_config(path)` | `mogbus_load_config` |
| `ctx.add_device(unit_id, name=None)`, `ctx.find_device(unit_id)`, `ctx.devices`, `iter(ctx)`, `len(ctx)` | `mogbus_add_device`, `mogbus_find_device`, `mogbus_device_at/count` |
| `dev.add_point(space, address, type="u16", value=0, word_order="high-first", ramp=None, byte_order="big", length=0, text=None, read_only=False)` | `mogbus_device_add_point` |
| `dev.read(space, address, count)`, `dev.write(space, address, values)` | `mogbus_device_read/write` (raw registers) |
| `dev.get_value(space, address)`, `dev.set_value(space, address, value)` | `mogbus_device_get_value/set_value` (decoded) |
| `dev.get_string(space, address)`, `dev.set_string(space, address, text)` | `mogbus_device_get_string/set_string` (string points) |
| `dev.info()` | `mogbus_device_info` |
| `ctx.serve(options=None, **overrides)`, `ctx.serve_config()` | `mogbus_server_create`, `mogbus_server_create_from_config` |
| `ctx.config_server_options()`, `ctx.check_server(...)` | `mogbus_config_server_options`, `mogbus_server_check` |
| `server.step(max_wait_ms)`, `.run()`, `.start()`, `.stop()`, `.close()` | `mogbus_server_step/run/stop/close` |
| `server.pty_name`, `server.metrics()` | `mogbus_server_pty_name`, `mogbus_server_metrics` |
| `probe(options, unit_id, timeout_ms)`, `probe_or_raise(...)`, `rtu_timing(baud, frame_gap_ms)` | `mogbus_probe`, `mogbus_rtu_timing` |
| `set_log_level`, `set_log_format`, `set_log_sink` | `mogbus_set_log_level/format/sink` |

## Names

Spaces (`"coil"`, `"discrete"`, `"input"`, `"holding"`), types (`"u16"`, `"s16"`, `"u32"`, `"s32"`,
`"f32"`/`"float32"`, `"bool"`, `"u64"`, `"s64"`, `"f64"`/`"float64"`, `"string"`), word orders
(`"high-first"`, `"low-first"`), byte orders (`"big"`, `"little"`), ramp modes (`"wrap"`, `"bounce"`,
`"hold"`), server modes (`"tcp"`, `"rtu-tcp"`, `"ascii-udp"`, ...) and the other enums are accepted as
strings or as the `mogbus.Space`, `mogbus.ValueType`, ... enum members (also plain ints). Anything
else raises `MogbusError` listing the valid names.

## Points and values

- `byte_order="little"` swaps the two bytes inside every register of the point; with `word_order` it
  gives the ABCD, CDAB, BADC and DCBA layouts (see the
  [config reference](../config/points/byte-order.md)).
- A `"string"` point needs `length` (registers, 1 to 123) and takes its initial `text`.
  `get_string()` returns the text up to the first NUL as `str` (Latin-1), `set_string()` replaces it.
  `get_value()` and `set_value()` raise on strings.
- `u64` and `s64` values are Python floats in `value`, so integers above 2^53 round; `dev.read()` is
  exact.
- `read_only=True` (coils and holding registers) makes Modbus writes fail with exception 02;
  `dev.write()` and `set_value()` still change the point.
- A ramp is a dict or `mogbus.Ramp`: `{"from": 0, "to": 100, "step": 10, "every_ms": 1000, "mode": "bounce"}`.
- `dev.write()` is what a client write does (it also writes `read_only` points, and fails on a point
  claimed by `set_value()`); `dev.set_value()` is the host side and makes the point host-owned. See
  [client write and host write](../library/library-api.md#client-write-and-host-write).

## Options, info and errors

- `ServerOptions` has the fields of `mogbus_server_options_t` in snake_case with the defaults of
  `mogbus_server_options_defaults()` (TCP on `127.0.0.1:1502`). Strings are encoded and kept alive for
  the duration of the C call; the library copies them.
- `dev.info()` returns a `DeviceInfo` with `name`, `unit_id`, `point_counts` per space, `faults` (only
  configured kinds, for example `"delay"`) and `custom_function_codes`.
- Errors raise `MogbusError` with the message of the C `err` buffer. A failed server creation also
  sets `error.status` (`mogbus.Status.INVALID`, `SERIAL`, `LISTEN`).
- `probe()` returns a bool; `probe_or_raise()` raises with the reason. Both block the caller and take
  no lock, so they work while a started server runs on its background thread.

## See also

- [Getting started with Python](python-start.md)
- [C API overview](../library/library-api.md)
