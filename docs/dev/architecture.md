# Architecture

## Data flow

```
 TCP       client ──[TLS] MBAP+PDU──────► net/tcp_server ─────────┐
 RTU       client ──addr+PDU+CRC────────► net/rtu_server ─────────┤
 RTU/TCP   client ──addr+PDU+CRC────────► net/rtu_tcp_server ─────┤
 ASCII     client ──:hex+LRC+CRLF───────► net/ascii_server ───────┼─► modbus/pdu ──► device/device ──► data tables
 ASCII/TCP client ──:hex+LRC+CRLF───────► net/ascii_tcp_server ───┤                        ▲
 UDP       client ──MBAP+PDU datagram───► net/udp_server ─────────┤          device_tick() │ ramps → registers
 RTU/UDP   client ──RTU frame datagram──► net/rtu_udp_server ─────┤
 ASCII/UDP client ──ASCII frame datagram► net/ascii_udp_server ───┘
                                                  │                              fault_decide_at()
                                                  ▼
     device/fault (delay / drop / exception / corrupt / disconnect /
                   restart / sensor-disconnect)

 config file ──► config/config ──► config/config_device ──► device/device
                  (s7 reads text)
```

0. `main` turns the loaded config into `server_options_t` (`server_config`) and runs `server`, which
   picks the transport below; a host can build the options itself and skip `config`.
1. At start-up, `config` reads the Scheme file with s7, **without evaluating it**, and builds
   `device_t` objects (data tables plus a list of points).
2. `net/tcp_server` (Modbus TCP MBAP, optionally TLS-wrapped via `net/net_tls`), `net/rtu_server` (RTU CRC-framed byte stream),
   `net/ascii_server` (ASCII hex/LRC-framed byte stream), `net/ascii_tcp_server` (the same ASCII
   framing over TCP), `net/rtu_tcp_server` (RTU CRC-framed over TCP), `net/udp_server` (one MBAP ADU per UDP datagram), `net/ascii_udp_server` (one ASCII frame per UDP datagram), or `net/rtu_udp_server` (one RTU frame per UDP datagram) finds the device by
   unit/address and calls `modbus_handle_pdu`.
3. The handler calls `device_tick` (ramps compute their value for "now" and write it into the
   tables), then reads or writes the tables.
4. `fault_decide_at` evaluates every effective fault kind (a percent roll or a schedule step) and the
   transport applies the combined decision (drop, delay, exception, pdu/register/checksum corrupt,
   or disconnect on TCP-like transports). Effective faults are device faults with overlays from the
   machines' active states; the earliest declared machine wins for each kind. A fired
   `sensor-disconnect` replaces successful FC 02/04 reads with exception 04 before sending, and a
   fired `restart` resets the device after the response is built. Schedule timing and limits are in
   [config-reference.md](../user/config-reference.md#fault-).

## Modules

| Folder | Files | Responsibility |
|---|---|---|
| `src/util` | `clock`, `rng`, `log`, `arena_impl` | monotonic milliseconds, reproducible dice, logging (stderr by default, or a host sink via `log_set_sink`; level via `log_set_level`) |
| `src/device` | `device`, `machine`, `behaviour`, `encode`, `fault` | the device model and compiled machine runtime; knows nothing about Modbus framing, TCP or Scheme |
| `src/modbus` | `mbap`, `pdu`, `rtu`, `ascii`, `bytes.h` | TCP MBAP, RTU CRC/frame codec, ASCII hex/LRC codec and delimiter scan, function codes, exceptions; knows nothing about sockets or Scheme |
| `src/net` | `tcp_server`, `net_tls`/`net_tls_stub`, `net_tcp_pool`, `rtu_server`, `ascii_server`, `ascii_tcp_server`, `rtu_tcp_server`, `udp_server`, `ascii_udp_server`, `rtu_udp_server`, `net_udp`, `rtu_framer`, `net_common` | TCP sockets, optional TLS for MBAP/TCP (OpenSSL wrapper or no-TLS stub), shared TCP accept/idle/poll connection pool, RTU byte stream, ASCII byte stream, ASCII-over-TCP, RTU-over-TCP, shared RTU rx/framing, `poll` loops, delayed sending, shared dispatch/helpers |
| `src/config` | `form`, `config_device`, `config_machine`, `config_fault`, `config` | s7 reading, option checking, error messages |
| `src/server` | `server`, `server_mode.h`, `server_limits.h`, `server_config` | transport-independent facade: `server_options_t`/`server_t` (`init/step/stop/close/serve/run`), PTY/serial open and baud auto-detect, TLS validation (`server_check_tls`); `server_config` is the only file that reads `config_t` |
| `src/api` | `mogbus.c`, `probe.c` | the public C API behind `include/mogbus.h` (see [library.md](../user/library.md)): context, devices and points, server handles, logging, and `mogbus_probe` (the `--health` client: one FC 08 echo over any network transport, built on the `modbus` codecs); the top layer, nothing includes it; public enums mirror the internal ones, checked with `_Static_assert` |
| `src/main.c` | | command line, signal handlers, `--check` summary, printing the PTY slave name; a pure client of `mogbus.h` (it includes nothing else) and linked against the static library, so the binary and an embedding host share one wiring path |

Dependencies point one way: `server → net → modbus → device → util`, `config → device → util`,
`server_config → server, config`, `main → api` (only through `include/mogbus.h`), and `api → server_config, server, config, net, modbus, device, util` (`net` only for the RTU timing helpers, `modbus` only for the probe's codecs). `config` includes only
`server/server_mode.h` (the transport enum) and `server/server_limits.h` (array limits and option defaults shared with `api`),
each in its own header so config does not pull in `net`, and `api` does not need config for them.
A host that builds `server_options_t` itself can drop `config/` and `server_config.c`: `WITH_CONFIG=0`
does exactly that (plus s7 and `main.c`); `api` then compiles its two config entry points as stubs that
fail with `built without config support`. `tools/check_deps.sh` enforces that only `server_config.{c,h}` includes `config/` and that `main.c` includes only `mogbus.h`.
`net_tls` wraps OpenSSL and keeps its headers out of other modules; `net_tls_ctx_t` is a
caller-owned struct holding the `SSL_CTX*` plus its own pool of 32 `net_tls_conn_t` sessions
(handshake/want flags and deadline), so there is no process-global TLS state. In a no-TLS build `net_tls_stub.c`
implements the same API without OpenSSL. `net_tcp_pool` owns only listener, accept, idle and
poll-table bookkeeping for the three TCP transports; per-connection framing/TLS setup stays in
the servers. `server_config` only supplies borrowed TLS strings through `server_options_t` and `config`
rejects `(tls ...)` at load time in no-TLS builds.
`build_config.h` provides the `WITH_TLS` and `WITH_CONFIG` fallbacks for direct compilations. `device` and `modbus`
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
  (the config file text, released right after loading). Each server (`tcp_server_t`, `rtu_server_t`, ...,
  including its TLS session pool) is a caller-owned struct: a stack local in `main`, but a host may use
  static or arena storage. It must not move between init and close.
- A library host gets one arena per `mogbus_t` context (`src/api`) holding devices and server slots; `mogbus_destroy` releases it.
- Request handling uses fixed-size stack buffers. Nothing is allocated per request.
- Exception: the s7 interpreter allocates internally. It exists only while the config is read
  and is freed with `s7_free` (s7 keeps a few hundred bytes of interned strings; that is
  s7's design and harmless for a one-shot read).
- `ARENA_IMPL` is defined in `src/util/arena_impl.c` only (so `libmogbus` is self-contained). The test runner (`vlib/vtest.c`) defines its own, so the test binary leaves `arena_impl` out.

## Why s7 reads data instead of running code

The config is read with `(read ...)` and walked as data. This keeps the user-facing format
simple, gives precise error messages, and means a config file cannot run arbitrary code.
Restricted `(machine ...)` forms are read by s7 at load time and compiled into flat
expression/statement arrays, including on-write handlers, per-state fault overlays and optional
state-register points; `s7_eval_c_string` is used only once to install a fixed read-all helper,
no user config form is evaluated, and request handling never touches s7.

## The network loops

### Server objects, signals and SIGPIPE

Every transport is a self-contained object with no process-global state, so one process can host
several servers and drive them from its own loop:

- `X_server_init(&s, &options)` sets up the listener/socket/TLS (0 on success, 2 if a TCP listener,
  a UDP socket or a TLS context could not be created) and `X_server_close(&s)` releases it (idempotent; devices and the
  RTU/ASCII fd stay the caller's).
- `X_server_step(&s, max_wait_ms)` runs one poll iteration (`-1` = until the server's next deadline,
  at most 1 s; `0` = nonblocking) and returns `NET_STEP_CONTINUE`, `NET_STEP_DONE` (stopped, or the
  RTU/ASCII peer closed the fd) or `NET_STEP_ERROR` (RTU/ASCII poll failure).
- `X_server_stop(&s)` only sets a `volatile sig_atomic_t`, so it is async-signal-safe; the next step
  returns `NET_STEP_DONE`. A signal that interrupts `poll()` makes the step return, so a run loop sees
  the flag at once.
- `X_server_run(&s, &options)` is init + step loop + close for the binary.

`server/server` wraps all eight transport servers behind one `server_t` selected by `server_mode_t`: `server_init`
opens the PTY/serial fd for the `*_pty`/`*_device` modes (and auto-detects a real tty's baud),
`server_pty_name` exposes the PTY slave path, `server_close` also closes fds it opened, and
`server_serve`/`server_run` are the step loop (exit 1 only on a poll error). The `*_fd` modes serve an
fd the host already owns (socketpair, PTY master) and never close it; the config file cannot select them.

The net layer installs no signal handlers. `main.c` owns SIGINT/SIGTERM (no `SA_RESTART`, calling
`mogbus_server_stop`, which only sets a flag, on the running server) and ignores SIGPIPE. Plain socket sends use `MSG_NOSIGNAL`; RTU/ASCII
byte-stream writes go to PTY/serial fds. OpenSSL writes to the socket with plain `write()`, so a host
embedding a TLS server must ignore SIGPIPE itself.

### Logging (`util/log`)

Logging is process-wide. By default INFO and WARN lines go to stderr with a UTC timestamp; the
binary's `--verbose` sets the DEBUG level. A host embedding the stack installs
`log_set_sink(fn, ctx)` to receive each formatted line (no trailing newline, truncated to 1023
bytes, valid only during the call) and chooses `log_set_level(LOG_LEVEL_DEBUG|INFO|WARN|OFF)`;
lines below the level are dropped before formatting. A `NULL` sink restores stderr.

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

### UDP (`net/udp_server.c`, `net/net_udp.c`)

`net/net_udp` is the shared datagram layer: one bound nonblocking socket (IPv4 or IPv6 literal, same
address parsing as the TCP listeners, `IPV6_V6ONLY` on IPv6), `recvfrom` with `MSG_TRUNC` so empty
datagrams and datagrams longer than the caller's buffer are consumed and dropped (DEBUG log), `sendto`
the peer, `ip:port` peer text for logs, and a fixed 16-slot table of delayed responses (peer address,
bytes, deadline) with `net_udp_flush_due` and `net_udp_poll_timeout`, so the `delay` fault never blocks
and nothing is allocated; a full table drops the new response with a WARN. It carries no framing, so
the RTU and ASCII servers over UDP reuse it unchanged (`NET_UDP_MAX_DATAGRAM` is the largest ASCII frame).

`net/udp_server` is a single-threaded `poll()` loop on that one socket (timeout = earliest delayed
response, at most 1 s) that reads up to 32 datagrams per step. One datagram is one complete MBAP ADU:
the protocol id must be 0 and the MBAP length must match the datagram size exactly, otherwise the
datagram is ignored (there is no stream to resynchronize or close). Unit routing, the unknown-unit
exception `0B`, PDU handling and fault decisions go through `net_mbap_respond`, shared with
`tcp_server`; the reply goes to the source address. There is no per-peer state, no idle timeout and no
disconnect fault. Selected by `(server (mode udp))`; UDP gives no delivery guarantee and the simulator
never retransmits.

`net/ascii_udp_server` has the same loop and reply path but frames with `src/modbus/ascii.c`: a datagram
is accepted only when `modbus_ascii_scan` reports one complete frame spanning the whole datagram and
`modbus_ascii_decode` accepts it, so there is no buffer, resync or partial-frame timeout. Every other
non-empty datagram up to `NET_UDP_MAX_DATAGRAM` counts one FC08 `0x000C` error on every device (as in
`ascii_tcp_server`); empty and longer datagrams are dropped by `net_udp` before it sees them. Broadcast
and unknown units are silent, the response is encoded uppercase, a `checksum` corrupt fault damages
only the LRC, and the encoded frame goes through `net_udp_send_at`. Selected by `(server (mode ascii-udp))`.

`net/rtu_udp_server` is the same loop over `net_udp` with RTU framing (selected by
`(server (mode rtu-udp))`). Each datagram (at most `MODBUS_RTU_MAX_FRAME` bytes; longer ones are
dropped by `net_udp_recv`) must satisfy `modbus_rtu_next_frame` with a frame length equal to the
datagram size; otherwise it is rejected whole and, at 4 bytes or more, counted as an FC 08 `0x000C`
communication error on every device. A valid frame goes through `rtu_framer_handle_frame` (shared
with the byte-stream and RTU-over-TCP framer: CRC check, broadcast handling, unit lookup, faults)
using one scratch `rtu_framer_t` in the server struct; the queued reply is moved into the
`net_udp` pending table (so `delay` never blocks) and sent to the source address. No timing state,
no per-peer state, no disconnect fault.

## Coding rules used here

- Plain C (gnu11), vlib types (`u8`, `u16`, `f64` ...), vlib macros (`ARRAY_COUNT`, `MIN`, `MAX`,
  `ARENA_*`, `KB`/`MB`).
- No inheritance-style tricks. Polymorphism is a table of `{name, function}` (see
  `DEVICE_ITEMS` in `config_device.c` and `EXPR_FORMS` in `config_machine.c`) or a `switch`.
- Guard clauses and early returns instead of nested `if`.
- Imperative, top-to-bottom functions that do the job they need to do; a helper used once is
  usually inlined. The full style rules are in `AGENTS.md`.
- No `malloc`/`free` in project code: vlib arenas only, and no allocation per request.
- Compiled with `-Wall -Wextra -Werror`.

## Build and tests

`make` builds `mogbus`; `make lib` builds `libmogbus.a` and `libmogbus.so` from the same
objects (compiled `-fPIC -fvisibility=hidden`, so only `MOGBUS_API` symbols are exported), and
`make lib-check` builds `examples/embed_tcp.c` against both and checks the exported symbols.
`make tests` builds both `mogbus` (used by integration tests)
and `mogbus-tests`, and `lib-check`, and runs the tests through `tools/run_tests.sh`: vtest always exits 0
for whole suites, so the script runs each test by
name and fails if any one fails. `tests/tcp_server.c` and the other `*_server.c` integration files start the real binary on a private
loopback port and exercise the documented protocol behaviour of each transport (routing, exceptions,
faults, connection limit, malformed frames and datagrams); override the binary with `MOGBUS_BIN`. Tests run in parallel with
`RUN_TESTS_JOBS` workers (default `nproc`) through `xargs -P`, one test per process; integration tests
pick free ports, so no two tests share a resource. The queue is ordered slowest first from the
durations of the previous run (`BIN.times`, or `RUN_TESTS_TIMES`); new tests go first.
`make asan` builds separate `.asan.o` objects and runs the tests under ASan + UBSan; s7 is
compiled without sanitizers because UBSan reports a misaligned load in its `local_strncmp`.
Compilations emit `.d` dependency files (`-MMD -MP`), so editing a header rebuilds every
object that includes it. CI (`.github/workflows/ci.yml`) runs these checks plus a `WITH_TLS=0` job
with no OpenSSL install step, which also runs `WITH_CONFIG=0` (and `WITH_CONFIG=0 WITH_TLS=0`) tests. The Makefile is based on the one `vlib/gen_makefile.py` creates, but finds
sources recursively because modules have sub-folders. `WITH_TLS=auto` (default) detects OpenSSL
via `pkg-config`; `WITH_TLS=1` requires it and fails early otherwise, while `WITH_TLS=0` compiles
only the no-TLS stub and omits OpenSSL flags/libraries. `WITH_CONFIG=1` (default) or `0`: with `0` the
Makefile skips `src/config/*`, `src/server/server_config.c`, `main.c` and s7, so there is no `mogbus`
binary, `make` builds the libraries, and `make tests` builds the test binary without the tests that need
the config loader or the binary (`config`, the `*_cli`, `tcp_server`, `*_tcp_server`, `udp_server`, `*_udp_server` and `machine_e2e` test files).
The `mogbus` binary is `main.o` linked with a static library of the same build variant (`libmogbus.a` in a prod build, else `libmogbus.dev.a`, `libmogbus.notls.asan.a`, ...; `make clean` removes them all), never with the `.so`. Object files are tagged per TLS (`.notls`) and config (`.nocfg`) setting, and `.build-mode` records
both, so switching configurations without a clean rebuild is safe. s7 is compiled with its own relaxed flags
(`-DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0`, so even s7's file and OS functions are off).

Release builds are reproducible: `REPRO_FLAGS` (`-ffile-prefix-map`), sorted source discovery,
`ar rcsD` and no embedded timestamps make `mogbus`, `libmogbus.a` and `libmogbus.so` identical across
build directories (`tools/check_reproducible.sh`, CI job `reproducible`). `make dist` packs the prod
build into a deterministic tarball and `tools/gen_sbom.sh` writes the SPDX SBOM; `release.yml` runs
both on tags and attests the results. `mogbus --version` prints the `mogbus_version()` string plus the
`git describe` revision, which only `main.o` receives. See [release.md](release.md).
The `Dockerfile` (digest-pinned Debian base, non-root UID 10001, `mogbus --health` check),
`deploy/docker-compose.yml` and `deploy/kubernetes/mogbus.yaml` are verified by the `docker` CI job;
see [deployment.md](../user/deployment.md).

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
  boundaries, which cannot reproduce byte-time-sensitive serial framing. RTU-over-TCP and
  RTU-over-UDP need no change because they are explicit separate transports.

### Operational observability

- Structured JSON logs and metrics for requests, errors, latency, active clients, device health
  and fault statistics. Why: automated test platforms need machine-readable status.
- Health/readiness and Prometheus endpoints. Why: container orchestration and monitoring need
  standard probes and scrape targets.

### Protocol validation and compatibility

- Independent Modbus conformance and interoperability testing against commercial PLCs/SCADA
  products. Why: supporting an FC does not prove interoperability.
- Fuzz and malformed/fragmented-frame tests, long-running stability and multi-client/device
  performance tests. Why: parser/transport robustness and capacity need evidence beyond unit
  tests.
