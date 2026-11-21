# AGENTS.md

## Build & verification
- `make` builds `./modbus-sim`.
- `make tests` builds and runs `./modbus-sim-tests` through `tools/run_tests.sh`.
  - Do not trust a plain whole-suite run of the vtest binary for pass/fail: vtest exits 0 for whole suites even on failure. `tools/run_tests.sh` runs each test individually and fails correctly.
- `make asan` builds/runs sanitizer tests (`ASAN_OPTIONS=detect_leaks=0` is intentional here; vendored `third_party/s7` is compiled without sanitizers). ASan builds include `float-cast-overflow`.
- CI-equivalent checks: `make`, `make tests`, `for p in profiles/*.scm; do ./modbus-sim --check "$p"; done`, `make asan`. CI also runs clang build/tests and a gcc `-fanalyzer` build. Keep all profile checks passing when changing config parsing/validation or device behavior.
- `./modbus-sim --check <config.scm>` validates a config without starting the server.

## Focused tests
- List tests: `./modbus-sim-tests --list`
- Run one test: `./modbus-sim-tests -s <suite> -t <test>`
- `tests/server.c` is integration coverage that starts the real server binary on loopback; override the binary with `MODBUS_SIM_BIN=/path/to/modbus-sim ./tools/run_tests.sh ./modbus-sim-tests`.

## Architecture & behavior (non-obvious)
- Main flow: `src/config/*` reads the Scheme config as data (via s7 `read`, not evaluation) and builds devices; `src/net/server.c` (single-thread `poll` loop, max 32 clients) dispatches requests through `src/modbus/*` to `src/device/*`. Dependency direction: `net -> modbus -> device -> util`, `config -> device -> util`. Headers live next to `.c` files. See `docs/architecture.md`.
- Device model: 4 Modbus tables of 65536 addresses each (coil/discrete/input/holding). Only configured points exist; an address with no owner is intentionally undefined and reads/writes to undefined/out-of-table targets return Modbus exception `0x02` (not a missing implementation). `device_tick` is computed at request time (no background thread).
- Modbus support is intentionally limited to TCP FC `01,02,03,04,05,06,07,08,0B,0C,0F,10,11,14,15,16,17,18` with exceptions `01,02,03,0B`; other FCs are unsupported by design. Writes are all-or-nothing and only valid for coils/holding registers and configured file records (FC 21). See `docs/modbus-support.md` and `docs/config-reference.md` before changing protocol/config semantics.

## Repo constraints
- Plain C (gnu11), Linux-only (`poll`, `accept4`, vlib mmap arenas). Default build is hardened with `-Wall -Wextra -Werror -Wformat=2 -Wshadow -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE` and linked with `-pie -Wl,-z,relro,-z,now`; `CC`, `CFLAGS`, and `LDFLAGS` can be overridden from the environment.
- No `malloc/free/new/delete` in project code; use vlib arenas (`ARENA_*`). `src/main.c` creates `app` (persistent) and `scratch` (released after config load); request handling must not allocate per request. `ARENA_IMPL` is defined only in `src/main.c` and `vlib/vtest.c`.
- `third_party/s7` and `vlib/` are vendored dependencies: do not “fix” style/warnings there; s7 is built with relaxed flags and excluded from sanitizers (known UBSan issue upstream in vendored version). `make commands` regenerates `compile_commands.json` via `bear` if needed.
- `src/*.c` are discovered recursively and `tests/*.c` by wildcard in the Makefile; adding files in those locations is picked up automatically.

## Style & scope
- Prefer the smallest change that solves the task; less is more. Avoid unnecessary abstraction, not optimization.
- Prefer fat structs over many small structs that are only used once inside a bigger struct. If a small struct belongs only to a bigger one, define it inside that bigger struct, e.g. `struct big { struct small { int a; }; /* other fields */ };`.
- Functions don't have to be short; they should do the job they need to do. If a small helper is only used once, prefer inlining it in the larger function. Inline tiny reused helpers when practical and clear.
- For enum-like groups of related constants, prefer enums over `#define` macros.
- Booleans don't need an `is_` prefix when the name is already clear (e.g. `log_verbose()`).
- Add comments only where logic is non-obvious or a constraint is easy to miss; do not comment routine changes. For complex behavior, add a short comment.
- When behavior changes or a new user-facing/config/protocol behavior is added, update the relevant docs (`docs/`, `README.md`, `AGENTS.md` as applicable) instead of adding explanatory comments everywhere.
- In reviews, inspect tests against intended behavior/spec; do not accept tests weakened or adapted to fit an incorrect implementation.
