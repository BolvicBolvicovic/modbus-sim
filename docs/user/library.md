# Using mogbus as a library

The `mogbus` binary is one front end; the same code builds as `libmogbus.a` and `libmogbus.so`
with a small C API, so a test harness or application can embed simulated Modbus devices and drive
them from its own loop. The only header is [`include/mogbus.h`](https://github.com/BolvicBolvicovic/mogbus/blob/main/include/mogbus.h) (it needs just
`<stdint.h>`, `<stdbool.h>` and `<stddef.h>`; every symbol is `mogbus_*` / `MOGBUS_*`). The current
version is 0.0.1 (`MOGBUS_VERSION_*`, `mogbus_version()`).

From Python, see [python.md](python.md) (a `ctypes` package over `libmogbus.so`; the same embedding as
`examples/embed_tcp.c` is in [`examples/python/embed.py`](../../examples/python/embed.py)).

## Build and link

```bash
make -j$(nproc) lib          # libmogbus.a + libmogbus.so
make -j$(nproc) lib-check    # also builds and runs examples/embed_tcp.c against both
```

`make` alone builds only the `mogbus` binary, which is itself a client of this API linked against
the static library. `make WITH_CONFIG=0` builds the libraries instead, since there is no binary
then. The `.a` contains the whole simulator, including the vendored s7 parser, so it is
self-contained. The `.so` exports only `mogbus_*` symbols; everything else is hidden.

```bash
# shared (found at run time via rpath or LD_LIBRARY_PATH)
cc -Iinclude app.c -L. -lmogbus -Wl,-rpath,'$ORIGIN' -o app
# static: also link what the library needs (-lm, plus -lssl -lcrypto in a TLS build)
cc -Iinclude app.c libmogbus.a -lm -lssl -lcrypto -o app
```

A `WITH_TLS=0` build has no OpenSSL dependency; creating a server with `tls_enabled` then fails
with an error. Objects are compiled with `-fPIC` (valid inside the PIE binary too), so one object
set serves the binary and both libraries.

### Build options

- `WITH_TLS=auto|0|1` (default `auto`): OpenSSL support, see the README.
- `WITH_CONFIG=1|0` (default `1`): `0` leaves out the config file loader (`src/config`,
  `src/server/server_config.c`) and the vendored s7, so the libraries hold no s7 code and serve only
  devices built in code. The header and the exported symbols are identical, so the ABI does not
  change: `mogbus_load_config()` returns false and `mogbus_server_create_from_config()` returns NULL,
  both with the error `built without config support`. A `WITH_CONFIG=0` build has no `mogbus` binary
  (it needs a config), so `make` builds `libmogbus.a`/`.so`. Combine freely with `WITH_TLS=0`.

## API overview

| Area | Calls |
|---|---|
| Context | `mogbus_create`, `mogbus_destroy`, `mogbus_load_config` |
| Devices | `mogbus_add_device`, `mogbus_find_device`, `mogbus_device_add_point`, `mogbus_device_count`, `mogbus_device_at`, `mogbus_device_info` |
| Values | `mogbus_device_read/write` (raw registers), `mogbus_device_get_value/set_value` (decoded), `mogbus_device_get_string/set_string` (string points) |
| Server | `mogbus_server_options_defaults`, `mogbus_server_create`, `mogbus_server_create_from_config`, `mogbus_server_step`, `mogbus_server_run`, `mogbus_server_stop`, `mogbus_server_close`, `mogbus_server_pty_name`, `mogbus_config_server_options`, `mogbus_server_check`, `mogbus_probe`, `mogbus_rtu_timing` |
| Logging | `mogbus_set_log_sink`, `mogbus_set_log_level`, `mogbus_set_log_format` |
| Observability | `http_bind_address`/`http_port`/`http_control` in `mogbus_server_options_t` (NULL bind = no endpoint; `http_control` also serves the unauthenticated `/api/` runtime control API), `mogbus_server_metrics` |

Introspection (used by the binary's `--check` summary): `mogbus_device_count()`/`mogbus_device_at()`
enumerate devices in order, and `mogbus_device_info()` fills a `mogbus_device_info_t` with name, unit id,
point counts per space and, per `mogbus_fault_kind_t`, whether the fault is configured, whether it fires by
schedule, its percent, delay and exception code. `custom_fc_count` and `custom_fcs[]` (up to 8 entries of
`code` and a borrowed `name`, `""` when unnamed) list the user-defined function codes of the device. These
come only from `(function-code ...)` in a config loaded with `mogbus_load_config`; the C API cannot add
them yet. `mogbus_config_server_options()` returns the loaded
`(server ...)` section as `mogbus_server_options_t` (strings borrowed from the context; false without a
config). `mogbus_server_check()` validates options exactly like create (device path, fd, serial line,
string lengths, and TLS files through a throwaway TLS context) without opening a listener, PTY or serial
device, so a port that is in use still passes. `mogbus_rtu_timing()` gives the t3.5 and the effective
inter-frame gap for a baud/frame-gap pair. Both create functions take an optional `mogbus_error_t* status`
(`MOGBUS_ERR_INVALID`, `MOGBUS_ERR_SERIAL` for a serial/PTY open failure, `MOGBUS_ERR_LISTEN` for a
listener or TLS failure); the binary maps serial to exit status 1 and listen/TLS to 2. A TLS setup failure is
also logged at WARN, like the transports log their own start-up errors.

`mogbus_probe(options, unit_id, timeout_ms, err, err_size)` is the binary's `--health`: it sends one FC 08
Return Query Data request to a unit of a server (this process's or another) and returns true for the echo or a
Modbus exception reply. It works for the TCP, UDP, RTU-over-TCP/UDP and ASCII-over-TCP/UDP modes, reaches
a `0.0.0.0` or `::` bind through loopback, only connects to a TLS server (no client certificate), and fails
with `health check needs a network mode` for serial/fd modes. It needs no context or config, uses blocking
sockets with a deadline and does not allocate, so it cannot probe a server that the calling thread
steps; call it from another thread or process. A fault that drops or delays replies on the probed unit
fails it.

A context holds up to `MOGBUS_MAX_DEVICES` (16) devices and `MOGBUS_MAX_SERVERS` (8) servers. A
server serves every device the context has when it is created. Points support static values and
ramps; machines and faults come only from config files (`mogbus_load_config`), and code-built and
loaded devices can be mixed. Every transport of the binary is a `mogbus_mode_t` in
`mogbus_server_options_t`:

| Modes | Options used |
|---|---|
| `MOGBUS_MODE_TCP` | `bind_address`, `port`, `idle_timeout_ms`, the `tls_*` fields |
| `MOGBUS_MODE_RTU_TCP`, `MOGBUS_MODE_ASCII_TCP` | `bind_address`, `port`, `idle_timeout_ms`; RTU also `baud`, `frame_gap_ms`, `broadcast_execute` |
| `MOGBUS_MODE_UDP`, `MOGBUS_MODE_RTU_UDP`, `MOGBUS_MODE_ASCII_UDP` | `bind_address`, `port`; RTU also `broadcast_execute` |
| `MOGBUS_MODE_RTU_PTY`, `MOGBUS_MODE_RTU_DEVICE`, `MOGBUS_MODE_ASCII_PTY`, `MOGBUS_MODE_ASCII_DEVICE` | `path` (device modes), the serial line fields, RTU timing |
| `MOGBUS_MODE_RTU_FD`, `MOGBUS_MODE_ASCII_FD` | `fd`: a byte stream you already own (a socketpair or PTY master), never closed by the library |

New modes are appended to the enum, so existing values keep their numbers. All modes use `seed`. Start from
`mogbus_server_options_defaults()`, which matches the config defaults (TCP on `127.0.0.1:1502`, 8N1).

`mogbus_device_write()` is what a client write does: it works on every space (the host plays the
physical side), turns a ramp static and runs machine on-write handlers, but fails on points a
machine drives. `mogbus_device_set_value()` is the host side of a point, like a machine output: it
also works on read-only spaces, but the point becomes host-owned, so ramps stop and client writes to
it get exception 02. Both address a point by its first address. Reads advance ramps to the current
time first.

## Lifetimes and threading

- Not thread-safe: one thread at a time uses a context. The thread that calls
  `mogbus_server_step()` is the one that may touch the devices. Only `mogbus_server_stop()` may be
  called from another thread or a signal handler.
- No `malloc`: memory is a `mmap`-backed arena owned by the context. Devices live until
  `mogbus_destroy()`; servers live until `mogbus_server_close()` (destroy closes the rest), and a
  closed server's slot is reused by the next create.
- Strings you pass in are copied; strings the library returns are borrowed. Errors are written to
  the optional `err`/`err_size` buffer.
- Logging is process-wide: `mogbus_set_log_sink()` receives each line synchronously on the logging
  thread; the default writes INFO/WARN lines to stderr.
- The library installs no signal handlers. TLS writes can raise SIGPIPE: ignore it in the host.
  `mogbus_server_stop()` is signal-safe, so a host handler can call it, as `main.c` does.

## Example

### Data types

`mogbus_point_t.type` is one of `MOGBUS_TYPE_U16/S16/U32/S32/F32/BOOL` and, appended later,
`MOGBUS_TYPE_U64/S64/F64` (4 registers) and `MOGBUS_TYPE_STRING`. `word_order` applies to 32- and
64-bit types; `byte_order` (`MOGBUS_BYTES_BIG`, the zero default, or `MOGBUS_BYTES_LITTLE`) swaps
the two bytes of every register of any register point. The two together give the ABCD, CDAB, BADC
and DCBA layouts listed in [config-reference.md](config-reference.md). Decoded access
goes through a `double`, so `u64`/`s64` above 2^53 round in `value`, `mogbus_device_get_value()`
and ramps; `mogbus_device_read/write` are exact.

A string point has `type = MOGBUS_TYPE_STRING`, `length` registers (1-123) and the initial `text`
(copied; NULL means empty; at most `2 * length` bytes, padded with NUL). It must be in the holding
or input table. `mogbus_device_get_string()` copies the text up to its first NUL into your buffer
(it fails when the buffer is smaller than the text plus the NUL) and `mogbus_device_set_string()`
replaces it, NUL padded. Unlike `mogbus_device_set_value()` the latter does not make the point
host-owned: clients can still write the registers. `mogbus_device_get_value()` and `set_value()`
return false on a string point. `byte_order`, `length` and `text` were appended to the struct, so
code built against the older header keeps working if it zero-initialises the struct.

[`examples/embed_tcp.c`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/embed_tcp.c) includes only `mogbus.h`. It creates a context,
adds a device with one holding register, starts a TCP server on a free loopback port, connects as a
raw MBAP client in the same process, calls `mogbus_server_step()` between client I/O, and checks the
value it reads back. `make lib-check` builds it against `libmogbus.so` (rpath `$ORIGIN/..`) and
`libmogbus.a` and runs both.

```c
mogbus_t* ctx = mogbus_create();
mogbus_device_t* dev = mogbus_add_device(ctx, 1, "embedded", err, sizeof(err));
mogbus_point_t point = { .space = MOGBUS_HOLDING, .address = 10, .type = MOGBUS_TYPE_U16, .value = 1234 };
mogbus_device_add_point(dev, &point, err, sizeof(err));

mogbus_server_options_t options = mogbus_server_options_defaults();
options.port = 1502;
mogbus_server_t* server = mogbus_server_create(ctx, &options, err, sizeof(err), NULL);
while (mogbus_server_step(server, 10) == MOGBUS_STEP_CONTINUE)
    /* host work here */;
mogbus_server_close(server);
mogbus_destroy(ctx);
```
