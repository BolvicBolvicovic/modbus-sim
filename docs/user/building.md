# Build from source

`make` builds `./mogbus`. This page lists the build options: compiler, optimization level, TLS and the config loader.

The `vlib/` directory is a git submodule: clone with `git clone --recurse-submodules`, or run
`git submodule update --init` in an existing clone (`make` stops with a clear error otherwise).

```bash
make -j$(nproc)        # builds ./mogbus (TLS enabled when OpenSSL headers are found)
```

Compilation is safe to parallelize, so `-j$(nproc)` gives a faster clean build.

## Compiler and linker

The default build prefers `clang`, falling back to `gcc` and then `cc`, and uses the system linker with
the hardening flags `-pie -Wl,-z,relro,-z,now`. `ccache` and `mold` are independent optional
speed-ups: `ccache` prefixes the chosen compiler when installed, and `-fuse-ld=mold` is used only when
mold is installed and the compiler accepts it (as in CI, where all three are present). An explicit
`CC` or `LDFLAGS` from the environment or command line wins (for example, `CC=gcc make`); hardening
flags are still appended unless already present.

## Optimization: `BUILD`

`BUILD` controls optimization (`prod` by default, matching CI):

| Value | Effect |
|---|---|
| `make` / `make BUILD=prod` | `-O2` with `_FORTIFY_SOURCE=2`; the hardened production build |
| `make BUILD=dev` | `-O0` without `_FORTIFY_SOURCE`; faster rebuilds, with separate object files and automatic relinking when switching modes |

The vendored s7 config parser is always built at `-O0`: it only runs at startup and dominates compile
time. `CFLAGS` still overrides the complete flag set if you need a custom build.

For the fastest local iteration, use `BUILD=dev` (or its `make dev` shorthand), which keeps the default
toolchain and uses separate `-O0` objects:

```bash
make -j$(nproc) dev          # BUILD=dev
make -j$(nproc) dev tests    # BUILD=dev, plus the test suite
```

## TLS: `WITH_TLS`

`WITH_TLS` controls the OpenSSL dependency (`auto` by default):

| Value | Effect |
|---|---|
| `make` / `make WITH_TLS=auto` | TLS enabled only when `pkg-config --exists openssl` succeeds; behavior is unchanged on systems with OpenSSL development files |
| `make WITH_TLS=1` | Require TLS; fail immediately with a clear error if `pkg-config` or OpenSSL development files are missing |
| `make WITH_TLS=0` | No OpenSSL cflags or libraries: builds every transport except TLS (plaintext TCP, UDP, RTU and ASCII); TLS tests are skipped |

A no-TLS build rejects `(tls ...)` at config load time with `built without TLS support`, including in
`--check` mode. Serial, RTU, ASCII, UDP and plaintext TCP configs behave identically in both builds.
`make WITH_TLS=0 tests` runs the no-TLS test set; `make WITH_TLS=0 asan` and
`make WITH_TLS=0 CC=clang` work as expected. Documentation examples containing `(tls ...)` are skipped
by `check_doc_examples.sh` when checking a no-TLS binary.

## Config loader: `WITH_CONFIG`

`WITH_CONFIG=1|0` (default `1`) controls the config file loader. `make WITH_CONFIG=0` compiles without
`src/config`, `src/server/server_config.c` and the vendored s7, and builds only `libmogbus.a` and
`libmogbus.so` (there is no `mogbus` binary); the config functions of the C API then fail with `built
without config support`. `make WITH_CONFIG=0 tests` runs the tests that do not need config or the
binary. See [Embedding mogbus in C](library/library-start.md).

## Libraries, Python, Windows

- `make lib` builds the static and shared libraries; see [Embedding mogbus in C](library/library-start.md).
- `make python` builds the package; see [Using mogbus from Python](python/python-start.md).
- `make PLATFORM=win32 WITH_TLS=0` cross-compiles the [native Windows build](windows/windows-native.md).

## Packaging and releases

Packaging, reproducible builds and the release process are maintainer topics, described in
[Release process](../dev/release.md). Running the tests and the other checks is described in
[Contributing](../../CONTRIBUTING.md).
