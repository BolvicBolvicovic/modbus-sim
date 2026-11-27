# Python API reference

The Python calls and the C functions they wrap, the accepted names for spaces and types, and how errors and values behave.

## Calls

| Python | C |
|---|---|
| `Context()`, `.close()`, `with` | `mogbus_create`, `mogbus_destroy` |
| `ctx.load_config(path)` | `mogbus_load_config` |
| `ctx.add_device(unit_id, name=None)`, `ctx.find_device(unit_id)`, `ctx.devices`, `iter(ctx)`, `len(ctx)` | `mogbus_add_device`, `mogbus_find_device`, `mogbus_device_at/count` |
| `dev.add_point(space, address, type="u16", value=0, word_order="high-first", ramp=None, byte_order="big", length=0, text=None, read_only=False, clock=None, clock_utc=False)` | `mogbus_device_add_point` |
| `dev.read(space, address, count)`, `dev.write(space, address, values)` | `mogbus_device_read/write` (raw registers) |
| `dev.get_value(space, address)`, `dev.set_value(space, address, value)` | `mogbus_device_get_value/set_value` (decoded) |
| `dev.get_string(space, address)`, `dev.set_string(space, address, text)` | `mogbus_device_get_string/set_string` (string points) |
| `dev.set_access_hook(fn)`, `dev.set_access_hook(None)` | `mogbus_device_set_access_hook` |
| `dev.set_any_unit(on=True)` | `mogbus_device_set_any_unit` (raises when another device has it) |
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
- `dev.info()` returns a `DeviceInfo` with `name`, `unit_id`, `any_unit`, `point_counts` per space, `faults` (only
  configured kinds, for example `"delay"`) and `custom_function_codes`.
- Errors raise `MogbusError` with the message of the C `err` buffer. A failed server creation also
  sets `error.status` (`mogbus.Status.INVALID`, `SERIAL`, `LISTEN`).
- `probe()` returns a bool; `probe_or_raise()` raises with the reason. Both block the caller and take
  no lock, so they work while a started server runs on its background thread.

## Access hook

`dev.set_access_hook(fn)` calls `fn(access)` on every Modbus client read or write of the device
(function codes and ordering as in the [C access hook](../library/library-access-hook.md)); `None`
removes it. `access` is a `mogbus.Access` with `kind` (`mogbus.AccessKind.READ` or `WRITE`), `space`
(`mogbus.Space`), `address`, `count`, `values` (list of ints for writes, `None` for reads) and
`function_code`, plus `target` (`mogbus.AccessTarget.TABLE`, `FILE_RECORD`, `FIFO` or `CANOPEN`) and the
fields the other targets use: `file` (file records; `address` is the first record), `node`, `index`,
`subindex` (CANopen; `address` is the byte offset and `count` the bytes) and `data` (`bytes` of a
CANopen write, else `None`). Check `target` first: a hook written for tables is also called for the
others. Return `None` (or 0) to continue, or an exception code (1-255) to answer it; a vetoed write
changes nothing, and a CANopen veto is answered as the failure `CE 0x08000020`. Host calls such as
`set_value()` and `write()` never reach it.

File records, FIFO queues and CANopen objects are built and changed from the host with
`dev.add_file_record(file, record, values)`, `read_file_records(file, record, count=1)`,
`write_file_records(file, record, values)`, `add_fifo(address, values=())`, `set_fifo(address, values=())`,
`add_canopen_object(node, index, subindex, data)`, `read_canopen(node, index, subindex, count, offset=0)`
and `write_canopen(node, index, subindex, data, offset=0)`; failures raise `MogbusError`.

```python
def hook(access):
    if access.kind == mogbus.AccessKind.READ:
        dev.set_value("holding", 0, read_sensor())   # refresh just before the client reads
    elif access.values[0] > 1000:
        return 3                                      # illegal data value

dev.set_access_hook(hook)
```

The hook runs on the server thread with the context lock held; the lock is reentrant, so calls on
the same device (or context) from inside it do not deadlock. An exception raised by the hook is
printed with its traceback and answered with exception 04.

## See also

- [Getting started with Python](python-start.md)
- [C API overview](../library/library-api.md)
