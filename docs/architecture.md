# Architecture

## Data flow

```
 TCP   client ──[TLS] MBAP+PDU─► net/tcp_server  ───────┐
                                                        ├─► modbus/pdu ──► device/device ──► data tables
 RTU   client ──addr+PDU+CRC───► net/rtu_server  ───────┤                        ▲
 ASCII client ──:hex+LRC+CRLF──► net/ascii_server       ├          device_tick() │ ramps → registers
 ASCII/TCP client ──same frames──► net/ascii_tcp_server ┘
                      │                                       fault_decide()
                      ▼
     device/fault (delay / drop / exception / corrupt / disconnect /
                   restart / sensor-disconnect)

 config file ──► config/config ──► config/config_device ──► device/device
                  (s7 reads text)
```

1. At start-up, `config` reads the Scheme file with s7, **without evaluating it**, and builds
   `device_t` objects (data tables plus a list of points).
2. `net/tcp_server` (Modbus TCP MBAP, optionally TLS-wrapped via `net/net_tls`), `net/rtu_server` (RTU CRC-framed byte stream),
   `net/ascii_server` (ASCII hex/LRC-framed byte stream), `net/ascii_tcp_server` (the same ASCII
   framing over TCP), or `net/rtu_tcp_server` (RTU CRC-framed over TCP) finds the device by
   unit/address and calls `modbus_handle_pdu`.
3. The handler calls `device_tick` (ramps compute their value for "now" and write it into the
   tables), then reads or writes the tables.
4. `fault_decide` evaluates every effective fault kind (a percent roll or a schedule step) and the
   transport applies the combined decision (drop, delay, exception, pdu/register/checksum corrupt,
   or disconnect on TCP-like transports). Effective faults are device faults with overlays from the
   machines' active states; the earliest declared machine wins for each kind. A fired
   `sensor-disconnect` replaces successful FC 02/04 reads with exception 04 before sending, and a
   fired `restart` resets the device after the response is built. Schedule timing and limits are in
   [config-reference.md](config-reference.md#fault-).

## Modules

| Folder | Files | Responsibility |
|---|---|---|
| `src/util` | `clock`, `rng`, `log` | monotonic milliseconds, reproducible dice, logging |
| `src/device` | `device`, `machine`, `behaviour`, `encode`, `fault` | the device model and compiled machine runtime; knows nothing about Modbus framing, TCP or Scheme |
| `src/modbus` | `mbap`, `pdu`, `rtu`, `ascii`, `bytes.h` | TCP MBAP, RTU CRC/frame codec, ASCII hex/LRC codec and delimiter scan, function codes, exceptions; knows nothing about sockets or Scheme |
| `src/net` | `tcp_server`, `net_tls`/`net_tls_stub`, `net_tcp_pool`, `rtu_server`, `ascii_server`, `ascii_tcp_server`, `rtu_tcp_server`, `rtu_framer`, `net_common` | TCP sockets, optional TLS for MBAP/TCP (OpenSSL wrapper or no-TLS stub), shared TCP accept/idle/poll connection pool, RTU byte stream, ASCII byte stream, ASCII-over-TCP, RTU-over-TCP, shared RTU rx/framing, `poll` loops, delayed sending, shared dispatch/helpers |
| `src/config` | `form`, `config_device`, `config_machine`, `config_fault`, `config` | s7 reading, option checking, error messages |
| `src/main.c` | | command line, wiring, RTU/ASCII PTY/device open |

Dependencies point one way: `net → modbus → device → util`, and `config → device → util`.
`net_tls` wraps OpenSSL and keeps its headers out of other modules; its opaque `net_tls_conn_t`
owns each session's handshake/want flags and deadline. In a no-TLS build `net_tls_stub.c`
implements the same API without OpenSSL. `net_tcp_pool` owns only listener, accept, idle and
poll-table bookkeeping for the three TCP transports; per-connection framing/TLS setup stays in
the servers. `config` only supplies borrowed TLS strings through `tcp_server_options_t` and
rejects `(tls ...)` at load time in no-TLS builds.
`build_config.h` provides the `WITH_TLS` fallback for direct compilations. `device` and `modbus`
can be tested alone, and are (see `tests/`).

Headers sit next to their `.c` files.

## Device model

- A `device_t` has four tables (`coil`, `discrete`, `input`, `holding`), each 65536 values.
- Each address stores which *point* defines it (`owner`). `owner == 0` means "not defined",
  which is how unknown addresses become exception 02.
- A `point_t` says where it lives, how it is encoded (`type`, `word-order`) and its behaviour
  (`POINT_STATIC`, `POINT_RAMP` or `POINT_MACHINE`).
- `ramp_value()` is a pure function of `(params, start, now)`. No state, so results are
  reproducible and tests need no clock.
- A client write turns the owning point static (`device_write`) only when it is not machine-owned;
  a write touching a machine-owned point fails all-or-nothing with exception 02. A successful write
  runs matching machine `on-write` handlers in the same request, after the raw values are applied.

## Memory

- No `malloc` or `free` in this code base. Everything lives in vlib arenas (`ARENA_ALLOC`, `ARENA_PUSH_STRUCT`, `ARENA_PUSH_ARRAY`).
- `main` creates two arenas: `app` (devices, lives until exit) and `scratch`
  (the config file text, released right after loading). Server state is a stack local.
- Request handling uses fixed-size stack buffers. Nothing is allocated per request.
- Exception: the s7 interpreter allocates internally. It exists only while the config is read
  and is freed with `s7_free` (s7 keeps a few hundred bytes of interned strings; that is
  s7's design and harmless for a one-shot read).
- `ARENA_IMPL` is defined in `main.c` only. The test runner (`vlib/vtest.c`) defines it for tests.

## Why s7 reads data instead of running code

The config is read with `(read ...)` and walked as data. This keeps the user-facing format
simple, gives precise error messages, and means a config file cannot run arbitrary code.
Restricted `(machine ...)` forms are read by s7 at load time and compiled into flat
expression/statement arrays, including on-write handlers, per-state fault overlays and optional
state-register points; `s7_eval_c_string` is used only once to install a fixed read-all helper,
no user config form is evaluated, and request handling never touches s7.

## The network loops

### TCP (`net/tcp_server.c`)

One thread, one `poll()` call. Listener setup, accept backoff, free-slot lookup, peer formatting
and idle sweeping are shared with the other TCP transports through `net/net_tcp_pool`; TLS
handshake state is queried through `net/net_tls`. Per connection there is one receive buffer and
one send buffer:

- A response with a delay fault is placed in the send buffer with a due time. The poll timeout
  is the nearest due time, so nothing sleeps.
- While a response is pending, the connection reads no further requests (they wait in the
  kernel), so responses stay in order.

### RTU (`net/rtu_server.c`)

Also single-threaded `poll()` on one bidirectional fd (PTY master, existing device, or similar byte
stream). `net/rtu_framer` owns the rx buffer, frame extraction, gap/t1.5 handling, and PDU dispatch
handoff; the server owns fd polling, EOF/hangup behavior, and delayed response writes. RTU has no MBAP
length field, so frame boundaries are detected by inter-frame silence. When a baud rate is known
(config/CLI or tty) and no explicit `frame-gap-ms` override is set, Modbus t3.5 is derived from baud
for inter-frame boundaries. Optional `strict_t15` enables a best-effort t1.5 intra-frame discard
heuristic in baud-derived auto mode for RTU byte streams. `frame-gap-ms` is an explicit inter-frame
override. With no known baud, `frame-gap-ms` or the 20 ms default is used as a practical approximation.
After a gap, complete valid CRC frames are parsed/handled; bad CRC, unknown unit, and broadcast (unit 0)
produce no response. Delayed fault responses use the same due-time poll pattern as TCP.

### ASCII (`net/ascii_server.c`)

Single-threaded `poll()` on one bidirectional byte stream (PTY master or existing device). It does
not use `net/rtu_framer`: `modbus/ascii` owns the hex/LRC codec and a delimiter scanner, and the
server owns the bounded RX buffer, fd polling, EOF/hangup behavior, and delayed response writes.
A frame is complete at `CR LF`, so complete frames are dispatched immediately without RTU timing;
several frames in one `read()` are handled in order. Malformed frames, bad LRC, broadcast, and
unknown units never reach PDU dispatch and produce no response. Fault drop suppresses the response;
delay faults reuse the due-time poll pattern. Serial line settings apply only here.

### ASCII-over-TCP (`net/ascii_tcp_server.c`)

Single-threaded `poll()` TCP listen loop with up to 32 connections. Each connection borrows the
devices, owns a bounded `MODBUS_ASCII_MAX_FRAME` RX buffer and one delayed-response slot, and uses
the same `modbus/ascii` scanner/codec as the serial ASCII server. Framing semantics are identical:
CRLF-delimited frames dispatch immediately, partial frames time out after 1 s, inter-frame
whitespace is skipped, a second colon resynchronizes, and malformed/bad-LRC frames count one FC08
`0x000C` error per device. Broadcast (unit 0) and unknown units are silent. `bind` supports IPv4/IPv6
literals and `idle-timeout-ms` closes idle clients. This is an explicit separate transport selected
by `(server (mode ascii-tcp))`; it is not Modbus TCP (MBAP) and does not alter serial ASCII behavior.

### RTU-over-TCP (`net/rtu_tcp_server.c`)

Single-threaded `poll()` TCP listen/server loop, similar to the TCP server, but each connection
carries Modbus RTU frames instead of MBAP. It reuses `net/rtu_framer` for shared RTU rx/framing,
`modbus/rtu` for CRC/frame codec and t3.5 timing helpers, and the shared PDU/fault dispatch path.
t1.5 discard is not enforced on RTU-over-TCP because TCP read() boundaries do not reflect UART
character timing. This is an explicit separate transport selected by `(server (mode rtu-tcp))`; it does not alter
TCP MBAP behavior or RTU byte-stream behavior.

## Coding rules used here

- Plain C (gnu11), vlib types (`u8`, `u16`, `f64` ...), vlib macros (`ARRAY_COUNT`, `MIN`, `MAX`,
  `ARENA_*`, `KB`/`MB`).
- No inheritance-style tricks. Polymorphism is a table of `{name, function}` (see
  `DEVICE_ITEMS` in `config_device.c`) or a `switch`.
- Guard clauses and early returns instead of nested `if`.
- Imperative, top-to-bottom functions, short, one job each.
- Compiled with `-Wall -Wextra -Werror`.

## Build and tests

`make` builds `mogbus`. `make tests` builds both `mogbus` (used by integration tests)
and `mogbus-tests`, and runs the tests through `tools/run_tests.sh`: vtest always exits 0
for whole suites, so the script runs each test by
name and fails if any one fails. `tests/tcp_server.c` starts the real binary on a private loopback
port and exercises the documented protocol behaviour (routing, exceptions, faults, connection
limit); override the binary with `MOGBUS_BIN`. Independent suites run in parallel with
`RUN_TESTS_JOBS` workers (default 3, capped by suite count); tests inside a suite stay sequential.
`make asan` builds separate `.asan.o` objects and runs the tests under ASan + UBSan; s7 is
compiled without sanitizers because UBSan reports a misaligned load in its `local_strncmp`.
Compilations emit `.d` dependency files (`-MMD -MP`), so editing a header rebuilds every
object that includes it. CI (`.github/workflows/ci.yml`) runs these checks plus a `WITH_TLS=0` job
with no OpenSSL install step. The Makefile is based on the one `vlib/gen_makefile.py` creates, but finds
sources recursively because modules have sub-folders. `WITH_TLS=auto` (default) detects OpenSSL
via `pkg-config`; `WITH_TLS=1` requires it and fails early otherwise, while `WITH_TLS=0` compiles
only the no-TLS stub and omits OpenSSL flags/libraries. Object files are tagged per TLS setting so
switching configurations without a clean rebuild is safe. s7 is compiled with its own relaxed flags
(`-DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0`, so even s7's file and OS functions are off).

The vendored s7 is version 10.6. To update, replace `third_party/s7/s7.c` and `s7.h`.

## Roadmap

Implemented behaviour is documented in the transport, device-model and config sections above;
only outstanding work is listed here.

### Dynamic behaviour models

- Add declarative `POINT_MODEL` entries for continuous dependencies (first-order lag, noise,
  sine, a heater following its setpoint). Why: machines and ramps can approximate smooth behavior
  only through hand-written tick logic; they cannot express declarative time/point-dependent
  models.
- Add gradual sensor degradation as a model variant. Why: drift and progressive failure should be
  configurable models, not bespoke per-test logic.

### RTU timing

- Model true UART character-level timing. Why: t3.5/t1.5 are derived from baud and read
  boundaries, which cannot reproduce byte-time-sensitive serial framing. RTU-over-TCP needs no
  change because it is an explicit separate transport.

### Operational observability

- Structured JSON logs and metrics for requests, errors, latency, active clients, device health
  and fault statistics. Why: automated test platforms need machine-readable status.
- Health/readiness and Prometheus endpoints. Why: container orchestration and monitoring need
  standard probes and scrape targets.

### Deployment and portability

- Docker image with deployment examples, container health checks, non-root execution and resource
  limits. Why: safe, repeatable deployment.
- Automated release builds with versioned binaries, reproducible builds and an SBOM. Why:
  traceable and verifiable binaries.

### Protocol validation and compatibility

- Independent Modbus conformance and interoperability testing against commercial PLCs/SCADA
  products. Why: supporting an FC does not prove interoperability.
- Fuzz and malformed/fragmented-frame tests, long-running stability and multi-client/device
  performance tests. Why: parser/transport robustness and capacity need evidence beyond unit
  tests.
