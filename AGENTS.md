# AGENTS.md

## Build & verification
- `make` builds `./modbus-sim`.
- `make tests` builds `./modbus-sim` (needed by integration tests) and `./modbus-sim-tests`, then runs the tests through `tools/run_tests.sh`.
  - Do not trust a plain whole-suite run of the vtest binary for pass/fail: vtest exits 0 for whole suites even on failure. `tools/run_tests.sh` runs each test individually and fails correctly. Independent suites run in parallel with `RUN_TESTS_JOBS` workers (default 3, capped by suite count); tests inside a suite stay sequential.
- `make asan` builds/runs sanitizer tests (`ASAN_OPTIONS=detect_leaks=0` is intentional here; vendored `third_party/s7` is compiled without sanitizers). ASan builds include `float-cast-overflow`.
- CI-equivalent checks: `make`, `make tests`, `for p in profiles/*.scm; do ./modbus-sim --check "$p"; done`, `make asan`. CI also runs clang build/tests and a gcc `-fanalyzer` build. Keep all profile checks passing when changing config parsing/validation or device behavior.
- `./modbus-sim --check <config.scm>` validates a config without starting the server.
- After every push, check CI (`gh run list`/`gh run watch`) and open GitHub issues (`gh issue list --state open`). If an open issue needs handling, finalize the current push, pause the current loop, handle issues sequentially (branch/worktree, implement/test/review/fix/reverify/merge/cleanup/push, checking CI and open issues after each push), then resume the previous loop.

## Focused tests
- List tests: `./modbus-sim-tests --list`
- Run one test: `./modbus-sim-tests -s <suite> -t <test>`
- `tests/tcp_server.c` is integration coverage that starts the real server binary on loopback; override the binary with `MODBUS_SIM_BIN=/path/to/modbus-sim ./tools/run_tests.sh ./modbus-sim-tests`.

## Architecture & behavior (non-obvious)
- Main flow: `src/config/*` reads the Scheme config as data (via s7 `read`, not evaluation) and builds devices. TCP (`src/net/tcp_server.c`, single-thread `poll`, max 32 clients), RTU (`src/net/rtu_server.c`, single-thread `poll` on one byte-stream fd), and RTU-over-TCP (`src/net/rtu_tcp_server.c`, single-thread `poll`, max 32 TCP clients, RTU framing) decode transport frames, then dispatch PDUs through `src/modbus/pdu.c` to `src/device/*`. `src/modbus/rtu.c` owns CRC-16 and RTU frame encode/decode. Dependency direction: `net -> modbus -> device -> util`, `config -> device -> util`. Headers live next to `.c` files. See `docs/architecture.md`.
- Device model: 4 Modbus tables of 65536 addresses each (coil/discrete/input/holding). Only configured points exist; an address with no owner is intentionally undefined and reads/writes to undefined/out-of-table targets return Modbus exception `0x02` (not a missing implementation). `device_tick` is computed at request time (no background thread).
- Modbus support is intentionally limited to FC `01,02,03,04,05,06,07,08,0B,0C,0F,10,11,14,15,16,17,18,2B/0E`. The simulator itself emits exception codes `01,02,03,0B`; the remaining standard codes (`04,05,06,07,08,0A`) are available only through `(fault (kind exception) (code N))` fault injection (`09` is reserved). Fault kinds are `none`, `delay`, `drop`, `exception`, `corrupt`; all faults affect only the response, never the request's processing. Other FCs are unsupported by design. TCP uses MBAP; RTU uses `[addr][PDU][CRC lo hi]` over PTY/device byte streams; RTU-over-TCP uses RTU frames over TCP sockets as an explicit separate `--rtu-tcp` transport. RTU inter-frame boundaries use baud-derived t3.5 when baud is known (config/CLI or tty), `frame-gap-ms` as an explicit override, or 20 ms default fallback when baud is unknown. In baud-derived auto mode, optional `strict-t15`/`--strict-t15` enables a best-effort intra-frame t1.5 discard heuristic for RTU byte streams only; it is never applied to RTU-over-TCP because TCP read() boundaries do not reflect UART character timing. Bad CRC, unknown RTU unit, and broadcast (0) produce no RTU response; opt-in `broadcast-execute`/`--broadcast-execute` executes unit-0 requests on every configured device (default off, still no response, RTU transports only — never MBAP/TCP). Writes are all-or-nothing and only valid for coils/holding registers and configured file records (FC 21). See `docs/modbus-support.md` and `docs/config-reference.md` before changing protocol/config semantics.

## Repo constraints
- Plain C (gnu11), Linux-only (`poll`, `accept4`, vlib mmap arenas). Default build is hardened with `-Wall -Wextra -Werror -Wformat=2 -Wshadow -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE` and linked with `-pie -Wl,-z,relro,-z,now`; `CC`, `CFLAGS`, and `LDFLAGS` can be overridden from the environment.
- No `malloc/free/new/delete` in project code; use vlib arenas (`ARENA_*`). `src/main.c` creates `app` (persistent) and `scratch` (released after config load); request handling must not allocate per request. `ARENA_IMPL` is defined only in `src/main.c` and `vlib/vtest.c`.
- `third_party/s7` and `vlib/` are vendored dependencies: do not “fix” style/warnings there; s7 is built with relaxed flags and excluded from sanitizers (known UBSan issue upstream in vendored version). `make commands` regenerates `compile_commands.json` via `bear` if needed.
- `src/*.c` are discovered recursively and `tests/*.c` by wildcard in the Makefile; adding files in those locations is picked up automatically.

## Style & scope
- Prefer the smallest change that solves the task; less is more. Avoid unnecessary abstraction, not optimization.
- Prefer fat structs over many small structs that are only used once inside a bigger struct. If a small struct belongs only to a bigger one, define it inside that bigger struct, e.g. `struct big { struct small { int a; }; /* other fields */ };`.
- Functions don't have to be short; they should do the job they need to do. If a small helper is only used once, prefer inlining it in the larger function. Inline tiny reused helpers when practical and clear.
- Multi-step `if`/`else if` conditions (three or more sub-conditions joined by `&&`/`||`) must be written one sub-condition per line, operator at the end of the line, continuation lines indented 4 spaces past the `if`:
  ```c
  if (a &&
      b &&
      c)
  ```
  A parenthesized group counts as one sub-condition.
- For enum-like groups of related constants, prefer enums over `#define` macros.
- Booleans don't need an `is_` prefix when the name is already clear (e.g. `log_verbose()`).
- Add comments only where logic is non-obvious or a constraint is easy to miss; do not comment routine changes. For complex behavior, add a short comment.
- When behavior changes or a new user-facing/config/protocol behavior is added, update the relevant docs (`docs/`, `README.md`, `AGENTS.md` as applicable) instead of adding explanatory comments everywhere.
- In reviews, inspect tests against intended behavior/spec; do not accept tests weakened or adapted to fit an incorrect implementation.
