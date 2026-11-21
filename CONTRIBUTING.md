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
for p in profiles/*.scm; do ./modbus-sim --check "$p"; done
make asan
```

`make tests` should be run through `tools/run_tests.sh` semantics as described in `AGENTS.md`; the Makefile target already does this.

## Coding rules

- Follow the constraints in [AGENTS.md](AGENTS.md): plain C (gnu11), Linux-only assumptions where already present, no `malloc/free/new/delete` in project code, and vlib arenas for project allocations.
- Do not style-fix or "clean up" vendored dependencies in `third_party/s7` or `vlib/`.
- Add comments only where logic is non-obvious or a constraint is easy to miss.
- When behavior/config/protocol semantics change, update the relevant docs.

## Pull requests

- Describe the problem, the chosen fix, and how you verified it.
- Include focused tests when fixing a bug or changing behavior.
