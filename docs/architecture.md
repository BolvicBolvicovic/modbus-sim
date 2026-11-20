# Architecture

## Data flow

```
 client ──TCP──► net/server ──(unit id, PDU)──► modbus/pdu ──► device/device ──► data tables
                     │                                              ▲
                     │ fault_decide()                  device_tick()│ ramps → registers
                     ▼
              device/fault (drop / delay)

 config file ──► config/config ──► config/config_device ──► device/device
                  (s7 reads text)
```

1. At start-up, `config` reads the Scheme file with s7, **without evaluating it**, and builds
   `device_t` objects (data tables plus a list of points).
2. For every request, `net/server` finds the device by unit id and calls `modbus_handle_pdu`.
3. The handler calls `device_tick` (ramps compute their value for "now" and write it into the
   tables), then reads or writes the tables.
4. `net/server` asks `fault_decide` whether to drop or delay the response.

## Modules

| Folder | Files | Responsibility |
|---|---|---|
| `src/util` | `clock`, `rng`, `log` | monotonic milliseconds, reproducible dice, logging |
| `src/device` | `device`, `behaviour`, `encode`, `fault` | the device model; knows nothing about Modbus framing, TCP or Scheme |
| `src/modbus` | `mbap`, `pdu`, `bytes.h` | frame header, function codes, exceptions; knows nothing about TCP or Scheme |
| `src/net` | `server` | sockets, `poll` loop, connections, delayed sending |
| `src/config` | `form`, `config_device`, `config` | s7 reading, option checking, error messages |
| `src/main.c` | | command line, wiring |

Dependencies point one way: `net → modbus → device → util`, and `config → device → util`.
`device` and `modbus` can be tested alone, and are (see `tests/`).

Headers sit next to their `.c` files.

## Device model

- A `device_t` has four tables (`coil`, `discrete`, `input`, `holding`), each 65536 values.
- Each address stores which *point* defines it (`owner`). `owner == 0` means "not defined",
  which is how unknown addresses become exception 02.
- A `point_t` says where it lives, how it is encoded (`type`, `word-order`) and its behaviour
  (`POINT_STATIC` or `POINT_RAMP`).
- `ramp_value()` is a pure function of `(params, start, now)`. No state, so results are
  reproducible and tests need no clock.
- A client write turns the owning points static (`device_write`).

## Memory

- No `malloc`, `free`, `new` or `delete` in this code base. Everything lives in vlib arenas
  (`ARENA_ALLOC`, `ARENA_PUSH_STRUCT`, `ARENA_PUSH_ARRAY`).
- `main` creates two arenas: `app` (devices, lives until exit) and `scratch`
  (the config file text, released right after loading). Server state is a stack local.
- Request handling uses fixed-size stack buffers. Nothing is allocated per request.
- Exception: the s7 interpreter allocates internally. It exists only while the config is read
  and is freed with `s7_free` (s7 keeps a few hundred bytes of interned strings; that is
  s7's design and harmless for a one-shot read).
- `ARENA_IMPL` is defined in `main.c` only. The test runner (`vlib/vtest.c`) defines it for tests.

## Why s7 reads data instead of running code

The config is read with `(read ...)` and walked as data. This keeps the user-facing format
simple, gives precise error messages, and means a config file cannot run code. When dynamic
models arrive (below), expressions can be evaluated with `s7_eval` in a controlled way.

## The network loop

One thread, one `poll()` call. Per connection there is one receive buffer and one send buffer:

- A response with a delay fault is placed in the send buffer with a due time. The poll timeout
  is the nearest due time, so nothing sleeps.
- While a response is pending, the connection reads no further requests (they wait in the
  kernel), so responses stay in order.

## Coding rules used here

- Plain C (gnu11), vlib types (`u8`, `u16`, `f64` ...), vlib macros (`ARRAY_COUNT`, `MIN`, `MAX`,
  `ARENA_*`, `KB`/`MB`).
- No inheritance-style tricks. Polymorphism is a table of `{name, function}` (see
  `DEVICE_ITEMS` in `config_device.c`) or a `switch`.
- Guard clauses and early returns instead of nested `if`.
- Imperative, top-to-bottom functions, short, one job each.
- Compiled with `-Wall -Wextra -Werror`.

## Build and tests

`make` builds `modbus-sim`. `make tests` builds `modbus-sim-tests` and runs it through
`tools/run_tests.sh`: vtest always exits 0 for whole suites, so the script runs each test by
name and fails if any one fails. `tests/server.c` starts the real binary on a private loopback
port and exercises the documented protocol behaviour (routing, exceptions, faults, connection
limit); override the binary with `MODBUS_SIM_BIN`.
`make asan` builds separate `.asan.o` objects and runs the tests under ASan + UBSan; s7 is
compiled without sanitizers because UBSan reports a misaligned load in its `local_strncmp`.
Compilations emit `.d` dependency files (`-MMD -MP`), so editing a header rebuilds every
object that includes it. CI (`.github/workflows/ci.yml`) runs all three checks.
The Makefile is based on the one `vlib/gen_makefile.py` creates, but finds sources recursively
because modules have sub-folders. s7 is compiled with its own relaxed flags
(`-DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0`, so even s7's file and OS functions are off).

The vendored s7 is version 10.6. To update, replace `third_party/s7/s7.c` and `s7.h`.

## Roadmap and where it plugs in

**Dynamic behaviour models.** Add a `POINT_MODEL` kind in `device/behaviour.c`: a function of
time and of other points (first-order lag, noise, sine, a heater that follows its setpoint).
The config gets a new entry next to `(static ...)` and `(ramp ...)`; `parse_behaviour` in
`config_device.c` is the only parser that changes. Models that need expressions can evaluate
them with s7 while the config is read, or keep an s7 environment alive and call it from
`device_tick`.

**More fault injection.** `fault_t` holds one fault today. Make it a small array and let
`fault_decide` return the combined decision (delay, drop, corrupt, wrong exception code,
disconnect). `net/server.c` already acts on a `fault_decision_t`, so new fields are
applied in one place (`handle_frame`).

**Many devices on one RTU bus through a virtual port.** The device table and unit-id routing
exist already (`find_device`). What is missing is a second transport:
1. `src/net/pty.c` (or `rtu.c`): open a pseudo-terminal (`posix_openpt`), print its path
   (for example `/dev/pts/7`) so a client can open it as a serial port.
2. `src/modbus/rtu.c`: RTU framing (unit id, PDU, CRC-16) and the silent-interval rules.
3. Feed `(unit, pdu)` into the same `modbus_handle_pdu` and the same `fault_decide`.
Bus-level behaviour (turnaround time, baud-rate delays, collisions) belongs in the transport,
not in the device model.
