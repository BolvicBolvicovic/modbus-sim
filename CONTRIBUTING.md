# Contributing

Small, focused changes are easiest to review.

## Before you start

- Read [AGENTS.md](AGENTS.md) for build commands, verification, architecture notes, and coding constraints.
- Keep changes consistent with the surrounding code and project scope.

## Development checks

Run the checks relevant to your change:

```bash
make
make tests
for p in profiles/*.scm; do ./mogbus --check "$p"; done
make asan
```

Locally `make tests` runs only the scopes your changes touch (`WHAT=auto`, the default; it prints the choice).
Use `WHAT=all` (or `c`, `python`, `docs`) to pick them yourself and `SUITES=obs,pdu` to limit the C tests to
some suites; CI always runs the full C suite and has separate Python and docs jobs.

`make tests` should be run through `tools/run_tests.sh` semantics as described in `AGENTS.md`; the Makefile target already does this.

Every test runs in its own process, one per CPU in parallel (set `RUN_TESTS_JOBS=N` to change),
slowest first: durations of the last run are kept in `mogbus-tests.times` / `mogbus-asan-tests.times`
(ignored by git, removed by `make clean`). `make asan` runs the tests under AddressSanitizer and UBSan
(the vendored s7 interpreter is excluded from instrumentation). The suite includes network integration
tests that start the real server on loopback; set `MOGBUS_BIN` to test a different binary.
`make fuzz-run` fuzzes the frame parsers, `tools/loadtest.py` load-tests a running server, and
`examples/interop/` checks mogbus against the independent pymodbus client stack (see
[Architecture](docs/dev/architecture.md)).

Build options (compiler, `BUILD`, `WITH_TLS`, `WITH_CONFIG`) are described in
[Build from source](docs/user/building.md).

## Coding rules

- Follow the constraints in [AGENTS.md](AGENTS.md): plain C (gnu11), Linux-only assumptions where already present, no `malloc/free/new/delete` in project code, and vlib arenas for project allocations.
- Do not style-fix or "clean up" `third_party/s7` or the `vlib/` submodule. Clone with `--recurse-submodules` (or run `git submodule update --init`); changes to vlib go upstream as pull requests to BolvicBolvicovic/vlib, then the submodule pin is bumped here.
- Add comments only where logic is non-obvious or a constraint is easy to miss.
- When behavior/config/protocol semantics change, update the relevant docs.

## Pull requests

- Describe the problem, the chosen fix, and how you verified it.
- Include focused tests when fixing a bug or changing behavior.
