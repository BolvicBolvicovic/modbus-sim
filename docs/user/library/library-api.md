# C API overview

The calls of `mogbus.h` by area, the server modes and what each option applies to, and the difference between a client write and a host write.

## Calls by area

| Area | Calls |
|---|---|
| Context | `mogbus_create`, `mogbus_destroy`, `mogbus_load_config` |
| Devices | `mogbus_add_device`, `mogbus_find_device`, `mogbus_device_add_point`, `mogbus_device_count`, `mogbus_device_at`, `mogbus_device_info`, `mogbus_device_set_any_unit` (also answer every unit id no other device has; one per context, see [any-unit](../config/device/any-unit.md)) |
| Values | `mogbus_device_read/write` (raw registers), `mogbus_device_get_value/set_value` (decoded), `mogbus_device_get_string/set_string` (string points) |
| File records, FIFO, CANopen | `mogbus_device_add_file_record`, `mogbus_device_read_file_records`, `mogbus_device_write_file_records`, `mogbus_device_add_fifo`, `mogbus_device_set_fifo`, `mogbus_device_add_canopen_object`, `mogbus_device_read_canopen`, `mogbus_device_write_canopen` ([host functions](library-access-hook.md#host-functions)) |
| Access hook | `mogbus_device_set_access_hook` ([run code on every client read or write](library-access-hook.md), tables, file records, FIFO and CANopen) |
| Server | `mogbus_server_options_defaults`, `mogbus_server_create`, `mogbus_server_create_from_config`, `mogbus_server_step`, `mogbus_server_run`, `mogbus_server_stop`, `mogbus_stop_on_signals`, `mogbus_server_close`, `mogbus_server_pty_name`, `mogbus_config_server_options`, `mogbus_server_check`, `mogbus_probe`, `mogbus_rtu_timing` |
| Logging | `mogbus_set_log_sink`, `mogbus_set_log_level`, `mogbus_set_log_format` |
| Observability | `http_bind_address`/`http_port`/`http_control` in `mogbus_server_options_t` (NULL bind = no endpoint; `http_control` also serves the unauthenticated `/api/` [runtime control API](../observability/api/control-api.md)), `mogbus_server_metrics` |

A context holds up to `MOGBUS_MAX_DEVICES` (16) devices and `MOGBUS_MAX_SERVERS` (8) servers. A
server serves every device the context has when it is created. Points support static values and
ramps; machines and faults come only from config files (`mogbus_load_config`), and code-built and
loaded devices can be mixed.

## Server modes

Every transport of the binary is a `mogbus_mode_t` in `mogbus_server_options_t`:

| Modes | Options used |
|---|---|
| `MOGBUS_MODE_TCP` | `bind_address`, `port`, `idle_timeout_ms`, the `tls_*` fields |
| `MOGBUS_MODE_RTU_TCP`, `MOGBUS_MODE_ASCII_TCP` | `bind_address`, `port`, `idle_timeout_ms`; RTU also `baud`, `frame_gap_ms`, `broadcast_execute` |
| `MOGBUS_MODE_UDP`, `MOGBUS_MODE_RTU_UDP`, `MOGBUS_MODE_ASCII_UDP` | `bind_address`, `port`; RTU also `broadcast_execute` |
| `MOGBUS_MODE_RTU_PTY`, `MOGBUS_MODE_RTU_DEVICE`, `MOGBUS_MODE_ASCII_PTY`, `MOGBUS_MODE_ASCII_DEVICE` | `path` (device modes), the serial line fields, RTU timing |
| `MOGBUS_MODE_RTU_FD`, `MOGBUS_MODE_ASCII_FD` | `fd`: a byte stream you already own (a socketpair or PTY master), never closed by the library |

New modes are appended to the enum, so existing values keep their numbers. All modes use `seed`.
Start from `mogbus_server_options_defaults()`, which matches the config defaults (TCP on
`127.0.0.1:1502`, 8N1).

## Client write and host write

`mogbus_device_write()` is what a client write does: it works on every space (the host plays the
physical side) and on points marked `read_only` (which Modbus clients cannot write), turns a ramp
static and runs machine on-write handlers, but fails on points a machine drives.

`mogbus_device_set_value()` is the host side of a point, like a machine output: it also works on
read-only spaces, but the point becomes host-owned, so ramps stop and client writes to it get
exception 02.

Both address a point by its first address. Reads advance ramps to the current time first.

## Introspection

`mogbus_device_count()` and `mogbus_device_at()` enumerate devices in order; the binary's `--check`
summary uses them.

- `mogbus_device_info()` fills a `mogbus_device_info_t` with the name, the unit id, the point counts
  per space and, per `mogbus_fault_kind_t`, whether the fault is configured, whether it fires by
  schedule, its percent, delay and exception code.
- `custom_fc_count` and `custom_fcs[]` (up to 8 entries of `code` and a borrowed `name`, `""` when
  unnamed) list the device's [user-defined function codes](../modbus/user-defined-function-codes.md). They
  come only from `(function-code ...)` in a config loaded with `mogbus_load_config`; the C API cannot
  add them yet.
- `mogbus_config_server_options()` returns the loaded `(server ...)` section as
  `mogbus_server_options_t` (strings borrowed from the context; false without a config).
- `mogbus_server_check()` validates options exactly like create (device path, fd, serial line, string
  lengths, and TLS files through a throwaway TLS context) without opening a listener, PTY or serial
  device, so a port that is in use still passes.
- `mogbus_rtu_timing()` gives the t3.5 and the effective inter-frame gap for a baud and
  frame-gap pair.

## Errors

Both create functions take an optional `mogbus_error_t* status`: `MOGBUS_ERR_INVALID`,
`MOGBUS_ERR_SERIAL` for a serial or PTY open failure, and `MOGBUS_ERR_LISTEN` for a listener or TLS
failure. The binary maps serial to exit status 1 and listen or TLS to 2. A TLS setup failure is also
logged at `WARN`, like the transports log their own start-up errors.

## Probing a server

`mogbus_probe(options, unit_id, timeout_ms, err, err_size)` is the binary's
[`--health`](../deployment/health-check.md): it sends one FC 08 Return Query Data request to a unit of
a server (this process's or another) and returns true for the echo or a Modbus exception reply.

- It works for the TCP, UDP, RTU-over-TCP/UDP and ASCII-over-TCP/UDP modes, reaches a `0.0.0.0` or
  `::` bind through loopback, only connects to a TLS server (no client certificate), and fails with
  `health check needs a network mode` for serial and fd modes.
- It needs no context or config, uses blocking sockets with a deadline and does not allocate, so it
  cannot probe a server that the calling thread steps: call it from another thread or process.
- A fault that drops or delays replies on the probed unit fails it.

## See also

- [Lifetimes and threading](library-lifetimes.md)
- [Data types and points](library-data.md)
- [Access hook](library-access-hook.md)
