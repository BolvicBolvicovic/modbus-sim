# AGENTS.md

## Build & verification
- `make` builds `./modbus-sim`.
- `make tests` builds and runs `./modbus-sim-tests` through `tools/run_tests.sh`.
  - Do not trust a plain whole-suite run of the vtest binary for pass/fail: vtest exits 0 for whole suites even on failure. `tools/run_tests.sh` runs each test individually and fails correctly.
- `make asan` builds/runs sanitizer tests (`ASAN_OPTIONS=detect_leaks=0` is intentional here; vendored `third_party/s7` is compiled without sanitizers).
- CI-equivalent checks: `make`, `make tests`, `for p in profiles/*.scm; do ./modbus-sim --check "$p"; done`, `make asan`. Keep all profile checks passing when changing config parsing/validation or device behavior.
- `./modbus-sim --check <config.scm>` validates a config without starting the server.

## Focused tests
- List tests: `./modbus-sim-tests --list`
- Run one test: `./modbus-sim-tests -s <suite> -t <test>`
- `tests/server.c` is integration coverage that starts the real server binary on loopback; override the binary with `MODBUS_SIM_BIN=/path/to/modbus-sim ./tools/run_tests.sh ./modbus-sim-tests`.

## Architecture & behavior (non-obvious)
- Main flow: `src/config/*` reads the Scheme config as data (via s7 `read`, not evaluation) and builds devices; `src/net/server.c` (single-thread `poll` loop, max 32 clients) dispatches requests through `src/modbus/*` to `src/device/*`. Dependency direction: `net -> modbus -> device -> util`, `config -> device -> util`. Headers live next to `.c` files. See `docs/architecture.md`.
- Device model: 4 Modbus tables of 65536 addresses each (coil/discrete/input/holding). Only configured points exist; an address with no owner is intentionally undefined and reads/writes to undefined/out-of-table targets return Modbus exception `0x02` (not a missing implementation). `device_tick` is computed at request time (no background thread).
- Modbus support is intentionally limited to TCP FC `01,02,03,04,05,06,07,08,0B,0C,0F,10,11,14,15` with exceptions `01,02,03,0B`; other FCs are unsupported by design. Writes are all-or-nothing and only valid for coils/holding registers and configured file records (FC 21). See `docs/modbus-support.md` and `docs/config-reference.md` before changing protocol/config semantics.

## Repo constraints
- Plain C (gnu11), Linux-only (`poll`, `accept4`, vlib mmap arenas), compiled with `-Wall -Wextra -Werror`.
- No `malloc/free/new/delete` in project code; use vlib arenas (`ARENA_*`). `src/main.c` creates `app` (persistent) and `scratch` (released after config load); request handling must not allocate per request. `ARENA_IMPL` is defined only in `src/main.c` and `vlib/vtest.c`.
- `third_party/s7` and `vlib/` are vendored dependencies: do not “fix” style/warnings there; s7 is built with relaxed flags and excluded from sanitizers (known UBSan issue upstream in vendored version). `make commands` regenerates `compile_commands.json` via `bear` if needed.
- `src/*.c` are discovered recursively and `tests/*.c` by wildcard in the Makefile; adding files in those locations is picked up automatically.
