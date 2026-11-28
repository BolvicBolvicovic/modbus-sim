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

0. `main` turns each loaded `(server ...)` into a `server_options_t` (`server_config`), creates one
   `mogbus_server_t` per section and runs them all with `mogbus_run` (one `os_wait` over the fds of every
   live server, `mogbus_step`); each `server` picks the transport below; a host can build the options
   itself and skip `config`. A server `id` (`(id "NAME")`) names a server; a device lists its servers
   with `(servers ...)` (`device_t.servers`, `device_serves()`), and a server serves only those devices.
   Unit ids are unique per server, which the config loader, `mogbus_device_add_server` and
   `mogbus_server_create` check.
1. At start-up, `config` reads the Scheme file with s7, **without evaluating it**, and builds
   `device_t` objects (data tables plus a list of points).
2. `net/tcp_server` (Modbus TCP MBAP, optionally TLS-wrapped via `net/net_tls`), `net/rtu_server` (RTU CRC-framed byte stream),
   `net/ascii_server` (ASCII hex/LRC-framed byte stream), `net/ascii_tcp_server` (the same ASCII
   framing over TCP), `net/rtu_tcp_server` (RTU CRC-framed over TCP), `net/udp_server` (one MBAP ADU per UDP datagram), `net/ascii_udp_server` (one ASCII frame per UDP datagram), or `net/rtu_udp_server` (one RTU frame per UDP datagram) finds the device by
   unit/address and calls `modbus_handle_pdu`.
3. The handler calls `device_tick` (ramps compute their value for "now" and write it into the
   tables), then reads or writes the tables. A function code with no built-in handler goes to
   `custom_respond` when the device defines it with `(function-code ...)` (`device_custom_fc_t`:
   cases matched by prefix on the request data), otherwise to exception 01.
4. `fault_decide_at` evaluates every effective fault kind (a percent roll or a schedule step) and the
   transport applies the combined decision (drop, delay, exception, pdu/register/checksum corrupt,
   or disconnect on TCP-like transports). Effective faults are device faults with overlays from the
   machines' active states; the earliest declared machine wins for each kind. A fired
   `sensor-disconnect` replaces successful FC 02/04 reads with exception 04 before sending, and a
   fired `restart` resets the device after the response is built. Schedule timing and limits are in
   [config reference](../user/config/faults/index.md).

## Project layout

```
include/        mogbus.h, the public C API of the library
examples/       embed_tcp.c (a host program using only mogbus.h), tutorial/ (pump_station.c, the C tutorial program), python/ (embed.py and a pytest example), interop/ (pymodbus interoperability check)
src/            the program, one folder per module (headers next to their .c files)
  build_config.h  WITH_TLS and WITH_CONFIG compile-time fallbacks shared by sources and tests
  config/       reads the config file, checks it, builds devices
  device/       data tables, points, ramps, value encoding, faults
  modbus/       Modbus TCP MBAP, RTU CRC/framing, ASCII hex/LRC framing, and PDU handlers
  webui/        the built-in browser page (index.html embedded as a byte array at build time)
  net/          TCP, RTU byte-stream, ASCII byte-stream, RTU-over-TCP, ASCII-over-TCP, UDP, ASCII-over-UDP and RTU-over-UDP network loops; OpenSSL TLS wrapper or no-TLS stub
  view/         the live register view (terminal table or lines) behind show_registers_ms
  api/          the public C API implementation (top layer)
  server/       transport-independent server facade and the config adapter
  util/         arena implementation, clock, random numbers, logging
profiles/       ready-made device profiles
tests/          unit tests and integration tests (vtest); tests/fuzz/ has the libFuzzer harnesses (make fuzz-run)
tools/          modbus_probe.py, loadtest.py, run_fuzz.sh, fuzz_seeds.py, check_deps.sh, check_doc_examples.sh, check_reproducible.sh, check_toolchain.sh, gen_sbom.sh, run_tests.sh
third_party/s7  the s7 Scheme interpreter (used only to read the config)
vlib/           git submodule: the vlib toolchain (arena allocator, test framework), unmodified
```

## Modules

| Folder | Files | Responsibility |
|---|---|---|
| `src/os` | `os_socket`, `os_wait`, `os_mutex`, `os_serial`, `os_time`, `os_console`, `os_error` (each with an `os_X_posix.c` and an `os_X_win32.c`) | the only code that includes OS headers: nonblocking TCP/UDP sockets and address text, `os_wait` (event wait over sockets: its entry type is the platform's own `struct pollfd`/`WSAPOLLFD`, and the call is `poll()` on POSIX, `WSAPoll` on Windows), a recursive mutex with caller-owned storage (`os_mutex`: pthread, `CRITICAL_SECTION`), serial/PTY open and termios line setup with the baud table, monotonic and UTC clocks, the SIGINT/SIGTERM stop hook and the stop-flag type, the terminal size and ANSI check (`os_console_terminal`, which also turns on VT processing on Windows), and last-error classification (`os_retry()`, `os_interrupted()`, ...) so no other file reads `errno` |
| `src/util` | `rng`, `log`, `arena_impl` | reproducible dice, logging (stderr by default, or a host sink via `log_set_sink`; level via `log_set_level`) |
| `src/device` | `device`, `machine`, `behaviour`, `encode`, `fault` | the device model and compiled machine runtime; knows nothing about Modbus framing, TCP or Scheme |
| `src/modbus` | `mbap`, `pdu`, `rtu`, `ascii`, `bytes.h` | TCP MBAP, RTU CRC/frame codec, ASCII hex/LRC codec and delimiter scan, function codes, exceptions; knows nothing about sockets or Scheme |
| `src/net` | `tcp_server`, `net_tls`/`net_tls_stub`, `net_tcp_pool`, `rtu_server`, `ascii_server`, `ascii_tcp_server`, `rtu_tcp_server`, `udp_server`, `ascii_udp_server`, `rtu_udp_server`, `net_udp`, `rtu_framer`, `net_common` | TCP sockets, optional TLS for MBAP/TCP (OpenSSL wrapper or no-TLS stub), shared TCP accept/idle/wait-table connection pool, RTU byte stream, ASCII byte stream, ASCII-over-TCP, RTU-over-TCP, shared RTU rx/framing, `poll` loops, delayed sending, shared dispatch/helpers |
| `src/webui` | `webui`, `index.html` | the web page served on `/` of the HTTP endpoint (a vanilla-JS client of `/api/`): `index.html` is turned into a C array by a Makefile rule (`od` and `awk`, no other tools) in the gitignored `page_data.h`; knows nothing of the other modules |
| `src/config` | `form`, `config_device`, `config_machine`, `config_fault`, `config` | s7 reading, option checking, error messages |
| `src/server` | `server` (with the transport enum `server_mode_t` and the limits and option defaults), `server_config` | transport-independent facade: `server_options_t`/`server_t` (`init/step/stop/close/serve/run`), PTY/serial open and baud auto-detect, TLS validation (`server_check_tls`); `server_config` is the only file that reads `config_t` |
| `src/view` | `view` | the live register view of a context (`show_registers_ms`): a terminal table redrawn in place (ANSI sequences, size from `os_console_terminal`, log lines kept in a pane below it through a log sink) or snapshot-then-changes text/JSON lines; renders into a buffer so tests can use a fake terminal size; knows only `device`, `util` and `os` |
| `src/client` | `client` | the Modbus client (master): one request/reply exchange over TCP, UDP or a serial line in every framing (MBAP, RTU, ASCII), lazy connect, reconnect after an error, deadlines and timeout retries; owns its arena and shares nothing with the servers; knows only the `modbus` codecs, `util` and `os` |
| `src/api` | `mogbus.c`, `probe.c`, `client_api.c` | the public C API behind `include/mogbus.h` (see [Embedding mogbus in C](../user/library/library-start.md)): context, devices and points, server handles, logging, `client_api.c` (the public client calls: function code encoding over `src/client`, `mogbus_client_*`), and `mogbus_probe` (the `--health` client: one FC 08 echo over any network transport, built on the `modbus` codecs); the top layer, nothing includes it; public enums mirror the internal ones, checked with `_Static_assert` |
| `src/main.c` | | command line, `--check` (quiet by default, `=verbose` prints the summary), printing the PTY slave name; a pure client of `mogbus.h` (it includes nothing else) and linked against the static library, so the binary and an embedding host share one wiring path |

Dependencies point one way: `server → net → modbus → device → util → os`, `net → webui` (webui includes only util and os), `config → device → util`, every layer may also use `os`, and `os` includes no other module (only vlib),
`server_config → server, config`, `view → device, util, os`, `main → api` (only through `include/mogbus.h`), and `client → modbus, util, os` (no TLS, no server, no device), and `api → client, server_config, server, view, config, net, modbus, device, util, os` (`net` only for the RTU timing helpers, `modbus` only for the probe's codecs). `config` includes only
`server/server.h` among the server headers (never `server_config.h`), for the transport enum `server_mode_t` and the array limits and
option defaults shared with `api`. `server.h` includes the `net` headers for the per-transport option structs it embeds, so config sees them
transitively; `tools/check_deps.sh` checks direct includes, and config itself uses nothing from `net`.
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
- A `point_t` says where it lives, how it is encoded (`type`, `word-order`, `byte-order`) and its
  behaviour (`POINT_STATIC`, `POINT_RAMP`, `POINT_MODEL` or `POINT_MACHINE`). `encode_bits()` lays
  a raw 64-bit pattern out in 1-4 registers (word order, then a byte swap in every register);
  `encode_value()` goes through it, so every type shares one layout code. Values are `f64` in
  ramps, models and machines, so `u64`/`s64` above 2^53 round there; a config `(static N)`
  integer on a `u64`/`s64` point is kept exactly in `static_raw` (`static_exact`) and used while
  the point is a plain static. `VT_STRING` points own `length` registers and a padded copy of
  their text in the device arena (`point_t.text`, 2*length bytes): `point_apply` rewrites the
  registers from it, so restart restores the configured text, client writes only change the
  table, and `device_get_point`/`device_set_point` (machines) refuse strings, which config
  rejects as machine targets, inputs, state registers and on-write watches.
- `ramp_value()` is a pure function of `(params, start, now)`. No state, so results are
  reproducible and tests need no clock.
- `POINT_MODEL` points (sine, lag, or a static value with noise) and ramps with noise are
  evaluated by `model_value()` in `behaviour.c`, the one place to add a modifier. Sine and the
  noise modifier are stateless (noise is a hash of unit id, point index and the sample slot
  through `rng_t`). A lag keeps `lag_y`/`lag_ms` in its `model_t` and advances by the exact
  discrete solution each `device_tick`; its target point is read from the tables at that
  moment. `device_reset_models()` restarts lags once the device is complete (a target may be
  declared later) and on `device_restart`; it also releases stuck values. `point_is_timed()` is what machines use to refuse
  ramp and model points.
- The degradation modifier is a struct in `model_t` (times in ms since `dev->start_ms`): drift
  and noise growth are added inside `model_value()` before the noise step, and stuck captures the
  final value at its first evaluation from `stuck_after_ms` (state reset by `model_reset()`).
  `fail-after-ms` is not a value: `device_range_failed()` scans the owners of a read range and
  the read handlers in `pdu.c` (`read_bits`, `read_registers`, FC 23 before it writes, the
  user-defined `(registers ...)` action) answer exception 04 after the address check. It uses the
  configured degradation whatever the point's current kind, so client writes and host claims do
  not clear it; only restart does. Because the exception goes through the normal response path it
  is counted like any other exception.
- A client write turns the owning point static (`device_write`) only when it is not machine-owned;
  a write touching a machine-owned point fails all-or-nothing with exception 02. A successful write
  runs matching machine `on-write` handlers in the same request, after the raw values are applied.

## Memory

- No `malloc` or `free` in this code base. Everything lives in vlib arenas (`ARENA_ALLOC`, `ARENA_PUSH_STRUCT`, `ARENA_PUSH_ARRAY`).
- `main` creates two arenas: `app` (devices, lives until exit) and `scratch`
  (the config file text, released right after loading). Each server (`tcp_server_t`, `rtu_server_t`, ...,
  including its TLS session pool) is a caller-owned struct: a stack local in `main`, but a host may use
  static or arena storage. It must not move between init and close.
- A library host gets one arena per `mogbus_t` context (`src/api`) holding devices and server slots; `mogbus_destroy` releases it. The context arena owns every server too (no arena per server): a device can serve several servers, so it must outlive them all.
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
- `X_server_stop(&s)` only sets an `os_stop_flag_t` (`volatile sig_atomic_t`), so it is async-signal-safe; the next step
  returns `NET_STEP_DONE`. A signal that interrupts `poll()` makes the step return, so a run loop sees
  the flag at once.
- `X_server_run(&s, &options)` is init + step loop + close for the binary.

`server/server` wraps all eight transport servers behind one `server_t` selected by `server_mode_t`: `server_init`
opens the PTY/serial fd for the `*_pty`/`*_device` modes (and auto-detects a real tty's baud),
`server_pty_name` exposes the PTY slave path, `server_close` also closes fds it opened, and
`server_serve`/`server_run` are the step loop (exit 1 only on a poll error). The `*_fd` modes serve an
fd the host already owns (socketpair, PTY master) and never close it; the config file cannot select them.

The net layer installs no signal handlers. `main.c` calls `mogbus_stop_on_signals(&g_server)`, which
(through `os_console_on_stop`) handles SIGINT/SIGTERM (no `SA_RESTART`, calling `mogbus_server_stop`,
which only sets a flag, on the running server) and ignores SIGPIPE. Plain socket sends use `MSG_NOSIGNAL`; RTU/ASCII
byte-stream writes go to PTY/serial fds. OpenSSL writes to the socket with plain `write()`, so a host
embedding a TLS server must ignore SIGPIPE itself.

### Threading

The public API is thread-safe per context and always on; there is no option. `mogbus_t` embeds one
recursive `os_mutex_t` and every public function that takes a context, device or server locks it for
its duration (`LOCK()` in `src/api/mogbus.c`, a `cleanup` attribute guard, so every return path
unlocks). The layers below the API (`server`, `net`, `device`) know nothing about threads except
two hooks for waiting: `net_poll()` (through `net_obs_t.lock`) and `server_group_step()` (through
`net_poll_group_t.lock`) unlock around `os_wait()` and lock again afterwards, so a host thread can
call `mogbus_device_*` while a server is idle, and a request is answered with the lock held. The lock
is recursive because hooks and callbacks run inside the step and may call `mogbus_device_*`.
`mogbus_server_run` and `mogbus_run` are loops of locked single steps, which also gives waiting host
threads a window between requests. Stopping must stay async-signal-safe, so `server_stop` takes no
lock: the stop flags (`os_stop_flag_t`) and `net_obs_t.ready` are atomics, and a step waiting in
`os_wait` notices within its timeout (at most 1 s). Not covered: `mogbus_destroy` and
`mogbus_server_close` racing other calls, a client used by two threads, and the process-wide log
settings (the log prefix and the machine parser's error text are `_Thread_local`). An always-on lock
was chosen over an option because an uncontended recursive mutex costs tens of nanoseconds per call
against the I/O of every request, and an option would add API surface and a mode where a second
thread silently corrupts a device. `tests/threads.c` covers it; run it with `-fsanitize=thread`
after touching the locking.

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

### Observability (`net/net_obs.c`)

Each server owns a `net_obs_t` (in `server_t`): counters updated by `net_handle_pdu()` and the
transports, the DEBUG request log line (text, or JSON fields via `log_write_fields`), and the optional
HTTP endpoint. Transports call `net_poll()` instead of `poll()`, which adds the endpoint's sockets to the
same poll set, so the endpoint runs on the server thread with no locking and no allocation.
`net_obs_http_feed()` takes the bytes of one request (head, then a `Content-Length` body of at most
1024 bytes) and builds the response; it is separate from the socket code so `tests/fuzz/fuzz_http.c`
can drive it. With `(http (control on))` the server sets `net_obs_t.control`, and `/api/` requests
go to `net_control_handle()` in `net/net_control.c`: routing, a flat JSON reader and writer, and
the `device_*` calls (including fault changes, validated by `fault_check()` in `device/fault.c`,
which shares its limits with the config loader). `net` must not include `config`, so the transport rules for
faults arrive as flags (`tcp_like`, `mbap`) set by `server_init()`.

## Coding rules used here

- Plain C (gnu11), vlib (a git submodule, unmodified here) types (`u8`, `u16`, `f64` ...), vlib macros (`ARRAY_COUNT`, `MIN`, `MAX`,
  `ARENA_*`, `KB`/`MB`).
- No inheritance-style tricks. Polymorphism is a table of `{name, function}` (see
  `DEVICE_ITEMS` in `config_device.c` and `EXPR_FORMS` in `config_machine.c`) or a `switch`.
- Guard clauses and early returns instead of nested `if`.
- Imperative, top-to-bottom functions that do the job they need to do; a helper used once is
  usually inlined. The full style rules are in `AGENTS.md`.
- OS calls live only in `src/os/`. Outside it no file may include `<poll.h>`, `<errno.h>`,
  `<signal.h>`, `<unistd.h>`, `<fcntl.h>`, `<termios.h>`, `<pty.h>`, `<netdb.h>`, `<strings.h>` or any
  `<sys/...>`, `<netinet/...>`, `<arpa/...>` header (`tools/check_deps.sh`); ISO C headers
  (`stdio.h`, `string.h`, `stdint.h`, `math.h`, `time.h`, `ctype.h`, ...) are fine, and `net_tls.c`
  may include OpenSSL. Errors are classified with `os_error.h`, not `errno`. One header per module
  (`os_X.h`) with its implementation in `os_X_<platform>.c`; `PLATFORM ?= posix` selects
  `os_*_posix.c`, `PLATFORM=win32` selects `os_*_win32.c` (see [Windows build](#windows-build)).
- No `malloc`/`free` in project code: vlib arenas only, and no allocation per request.
- Compiled with `-Wall -Wextra -Werror`.
- Doc comments use Doxygen: every source file starts with `/** @file @brief ... */`; a type,
  function or macro gets `/** @brief One sentence. Details. */` above it; members get `/**< ... */`
  after them (or `/** ... */` above); related declarations share a `@name ... @{ ... @}` group.
  Public functions in `include/mogbus.h` document every `@param` and `@return`; internal ones add
  them only when the brief does not already say it. Comments inside function bodies stay `/* */`.
  `make docs` (CI job `docs`) runs Doxygen with warnings as errors, then
  `tools/check_doc_briefs.sh`, which fails for any file, struct, union, enum or opaque or
  function-pointer typedef without a brief, then builds the site (see [Documentation site](docs.md)).
- Typedef layout: forward typedefs (`typedef struct x_t x_t;`, enums and unions alike) are grouped
  at the top of the file after the includes; the brief sits directly above the `struct x_t {` or
  `enum x_t {` definition (a brief above the typedef would be attached to the typedef, not the
  type). Never use the combined `typedef struct x_t { ... } x_t;` form, also not in
  `include/mogbus.h`. Anonymous enums keep their brief directly above them; function-pointer
  typedefs and opaque typedefs keep their own brief above the typedef.

## Build and tests

Robustness evidence beyond the unit tests:

- **Fuzzing:** `tests/fuzz/fuzz_{pdu,mbap,rtu,ascii}.c` are libFuzzer harnesses (ASan + UBSan) for the PDU
  handler with a device that has every feature, the MBAP request/response path, RTU codec and stream
  framer (fed in fuzzer-chosen chunks with simulated gaps), and ASCII scanning/decoding. Each checks
  invariants (response sizes, echoed function codes, encode/decode round trips). `make fuzz` builds
  them in `build/fuzz/`, `make fuzz-run FUZZ_SECONDS=N` seeds them with one valid request per FC
  (`tools/fuzz_seeds.py`) and runs each for N s, in parallel (at most `FUZZ_JOBS`, default `nproc`); the CI `fuzz` job runs
  30 s each per push, restoring and saving the corpus (`build/fuzz/corpus`) with `actions/cache`, and
  uploads crash reproducers. `.github/workflows/fuzz-nightly.yml` runs 600 s daily on the same corpus and
  opens (or comments on) the issue "Nightly fuzzing found a crash" on failure. Replay one with `build/fuzz/fuzz_NAME FILE`.
- **Malformed and fragmented frames:** `tests/robustness.c` runs every network transport in-process
  through the API: a request sent one byte at a time is answered, and after seeded random garbage
  (biased to frame-start bytes) in random chunks or datagrams, and frames split across datagrams,
  the server still answers a valid request.
- **Load and stability:** `tools/loadtest.py` runs N concurrent TCP clients for a duration and reports
  throughput, latency percentiles, errors and the server's RSS (start, peak, end); it fails on any
  error or memory growth. CI runs 32 clients for 5 s; run it for hours for a soak test. On a laptop
  the Python client tops out near 26 000 req/s against one mogbus with flat RSS (about 10 MB).
- **Interoperability:** `examples/interop/pymodbus_interop.py` checks every supported FC through
  pymodbus on all six network transports (CI job `interop`).

`make` builds `mogbus`; `make lib` builds `libmogbus.a` and `libmogbus.so` from the same
objects (compiled `-fPIC -fvisibility=hidden`, so only `MOGBUS_API` symbols are exported), and
`make lib-check` builds `examples/embed_tcp.c` and `examples/tutorial/pump_station.c` against both, runs them and checks the exported symbols.
`make tests` builds both `mogbus` (used by integration tests)
and `mogbus-tests`, and `lib-check`, and runs the tests through `tools/run_tests.sh`: vtest always exits 0
for whole suites, so the script runs each test by
name and fails if any one fails. `tests/tcp_server.c` and the other `*_server.c` integration files start the real binary on a private
loopback port and exercise the documented protocol behaviour of each transport (routing, exceptions,
faults, connection limit, malformed frames and datagrams); override the binary with `MOGBUS_BIN`. Tests run in parallel with
`RUN_TESTS_JOBS` workers (default `nproc`) through `xargs -P`, one test per process; integration tests
pick free ports, so no two tests share a resource. The queue is ordered slowest first from the
durations of the previous run (`BIN.times`, or `RUN_TESTS_TIMES`); new tests go first.
`make tests` is a dispatcher over the scopes in `WHAT` (`c`, `python`, `docs`, `all`, `auto`): `c` is the run
described above, `python` runs `python-tests` and `python-examples`, `docs` runs `make docs` and
`tools/check_doc_examples.sh`. `auto` (the local default) chooses the scopes from the files changed versus the
merge base with `origin/main` plus uncommitted and untracked files (`tools/test_scope.sh`, which prints its
choice); when the `CI` environment variable is set the default is `c`, and CI jobs for Python and docs keep their
own targets. `SUITES=a,b` limits the `c` scope to those vtest suites (`RUN_TESTS_SUITES` for `run_tests.sh`).
`make asan` builds separate `.asan.o` objects and runs the tests under ASan + UBSan; s7 is
compiled without sanitizers because UBSan reports a misaligned load in its `local_strncmp`.
Compilations emit `.d` dependency files (`-MMD -MP`), so editing a header rebuilds every
object that includes it. CI (`.github/workflows/ci.yml`) starts only for code paths, and its
`changes` job (`tools/ci_changes.sh`) skips the build and test jobs when C files changed only in
comments or only packaging files changed. It runs these checks plus a `WITH_TLS=0` job
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
`git describe` revision, which only `main.o` receives. See [Releases](release.md).
The `Dockerfile` (digest-pinned Debian base, non-root UID 10001, `mogbus --health` check),
`deploy/docker-compose.yml` and `deploy/kubernetes/mogbus.yaml` are verified by the `docker` CI job;
see [Run mogbus in Docker](../user/deployment/docker.md).

The vendored s7 is version 10.6. To update, replace `third_party/s7/s7.c` and `s7.h`.

### Windows build

`make PLATFORM=win32 WITH_TLS=0` cross-compiles a native Windows build on Linux with MinGW-w64 GCC
(GNU C extensions rule out MSVC); it covers the network modes only. Users get it through
[Windows](../user/windows/windows-route.md), which also documents Docker Desktop and WSL2 (including
serial adapters through usbipd-win).

- `src/os/os_*_win32.c`: `os_error` (`WSAGetLastError`, `FormatMessage`), `os_socket` (Winsock 2:
  `WSAStartup` on every socket creation, since it is reference counted; `ioctlsocket` `FIONBIO` plus a
  non-inheritable handle for nonblocking and close-on-exec; `SO_EXCLUSIVEADDRUSE` instead of
  `SO_REUSEADDR`; UDP `SIO_UDP_CONNRESET` off and `WSAEMSGSIZE` reported as cap + 1), `os_wait`
  (`WSAPoll` over sockets in 50 ms slices, so the console handler's interrupt flag is seen like
  `EINTR`; serial entries are flagged invalid), `os_time` (`QueryPerformanceCounter`,
  `GetSystemTimePreciseAsFileTime`) and `os_console` (`SetConsoleCtrlHandler`). `os_socket_t` is the
  handle type (`uintptr_t` on Windows), compared only through `os_socket_valid()` and
  `OS_SOCKET_INVALID` outside `src/os`.
- `os_serial_win32.c` is a stub (`os_serial_supported()` is false): serial ports, PTYs and the `*_fd`
  modes fail with a clear message.
- vlib arenas use their `VirtualAlloc` backend.
- Toolchain: the MinGW `printf` format attribute is the old MSVCRT dialect, so `-Wno-format` applies
  to this platform and the Linux build keeps the format checks; `_FORTIFY_SOURCE=2` and
  `-fstack-protector-strong` link against MinGW's static libssp; `mogbus.exe` links `-static` with
  `--dynamicbase --nxcompat --high-entropy-va`, next to `libmogbus.win32.a`, `mogbus.dll` (+
  `libmogbus.dll.a`) and `mogbus-tests.exe`. TLS is not ported (`net_tls.c` is POSIX only, Windows
  builds use `net_tls_stub.c`).
- Tests: `TESTS_POSIX_ONLY` in the Makefile lists the tests that fork, use PTYs/termios/`poll()` or
  spawn the binary through `test_support.c`; `make PLATFORM=win32 tests` runs the rest under Wine.
  The CI job `windows-cross` runs `tools/win32_check.sh`: cross build, unit tests under Wine,
  `mogbus.exe` in all six network modes with `--health`, `modbus_probe.py` and pymodbus interop.

## Roadmap

Outstanding work is listed in [Roadmap](roadmap.md).
