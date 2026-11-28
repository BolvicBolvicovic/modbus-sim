# Releases

A release is a tag `vX.Y.Z` whose version equals `MOGBUS_VERSION_MAJOR/MINOR/PATCH` in
`include/mogbus.h` (the single source of truth; `mogbus_version()` and `mogbus --version` read it).
`.github/workflows/release.yml` builds, tests and publishes everything from that tag.

## Cutting a release

1. Bump `MOGBUS_VERSION_*` in `include/mogbus.h`, merge to `main` once CI is green.
2. `git tag -a vX.Y.Z -m vX.Y.Z && git push origin vX.Y.Z`.
3. The workflow fails immediately if the tag and the header disagree. Otherwise, on `ubuntu-24.04`
   (x86_64) it runs `make tests`, then `make dist` with
   `SOURCE_DATE_EPOCH` set to the tagged commit time. The `publish` job then writes a combined
   `SHA256SUMS`, creates build provenance attestations (`actions/attest-build-provenance`) and the
   GitHub release (`gh release create --generate-notes`).
4. The `build-windows` job (`windows-latest`, MSYS2 MinGW-w64) runs the Windows tests and
   `make PLATFORM=win32 CROSS= WITH_TLS=0 dist`; its zip and SBOM join the same `SHA256SUMS`
   and attestations.
5. Dry run: start the workflow manually (`workflow_dispatch`); it builds and uploads workflow
   artifacts only and publishes nothing.

Releases are x86_64 only for now: the test framework (`vlib/vtest.h`) uses x86
intrinsics, so the tests cannot run on aarch64 and untested binaries are not published. Release
assets per architecture: `mogbus-X.Y.Z-linux-<arch>.tar.gz` and
`mogbus-X.Y.Z-linux-<arch>.spdx.json`, plus one `SHA256SUMS`; for Windows `mogbus-X.Y.Z-windows-x86_64.zip`
and `mogbus-X.Y.Z-windows-x86_64.spdx.json` (see [Windows zip](#windows-zip)).

## Tarball layout

`make dist` (prod build only; `BUILD=dev`, `SAN=1` and `WITH_CONFIG=0` are refused) writes
`dist/mogbus-<version>-linux-<arch>.tar.gz`, `dist/<same>.spdx.json` and `dist/SHA256SUMS`
(`<arch>` is `uname -m`). The tarball has one top directory `mogbus-<version>-linux-<arch>/` with
`bin/mogbus`, `lib/libmogbus.a`, `lib/libmogbus.so`, `lib/pkgconfig/mogbus.pc`, `include/mogbus.h`,
`share/doc/mogbus/` (README.md, LICENSE, NOTICE.md, the user docs `docs/user/`) and `share/mogbus/profiles/*.scm`.
`make clean` removes `dist/`. The tree is staged with `make install DESTDIR=... PREFIX= PC_RELOCATABLE=1` (see
[Install](../user/building.md#install)), so the tarball and a normal install cannot drift apart;
`PC_RELOCATABLE=1` writes `prefix=${pcfiledir}/../..` into `mogbus.pc` so the unpacked tree is usable
from anywhere. The shared library keeps the soname `libmogbus.so` until 1.0 (a versioned soname would
need `libmogbus.so.N` plus a symlink in the build tree, the rpath examples and the Python loader).
`tools/check_install.sh` (run by `make lib-check`) installs into a temporary `DESTDIR`, builds
`examples/embed_tcp.c` against it through `pkg-config --cflags --libs mogbus` (shared and `--static`),
runs both and checks that `make uninstall` leaves no file.

The binary links OpenSSL dynamically (`libssl.so.3`, `libcrypto.so.3`, glibc): install `libssl3`
(Debian/Ubuntu) on the target. Use a `WITH_TLS=0` source build for a binary without OpenSSL.

`mogbus --version` prints `mogbus X.Y.Z (<git describe>)`; the revision is passed only to `main.c`
and is omitted outside a git checkout (`GIT_REVISION=` also disables it). A tagged commit gives
`vX.Y.Z`, a dirty tree appends `-dirty`.

## Windows zip

`make PLATFORM=win32 CROSS= WITH_TLS=0 dist` (MSYS2 MinGW-w64 shell with Python; `BUILD=prod`,
`WITH_CONFIG=1`, `WITH_TLS=0` required) writes `dist/mogbus-<version>-windows-x86_64.zip`, its
`.spdx.json` and `dist/SHA256SUMS`. The zip has one top directory with `bin/` (`mogbus.exe`,
`mogbus.dll`), `lib/` (`libmogbus.dll.a`, and `libmogbus.win32.a` renamed `libmogbus.a`), `include/mogbus.h`,
`profiles/`, `docs/` (the user docs), `README.md`, `LICENSE` and `NOTICE.md`. It is reproducible for one
toolchain: `tools/make_zip.py` sorts the entries and fixes timestamps (`SOURCE_DATE_EPOCH`), modes and
compression level (the bytes still depend on the zlib of the Python that writes it), and the PE links
pass `--no-insert-timestamp`. The SBOM lists the imported system DLLs (`ws2_32.dll`, ...) instead of
`DT_NEEDED` entries. Windows TLS is not shipped (the zip is a `WITH_TLS=0` build).

## Verifying a download

```bash
sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify mogbus-X.Y.Z-linux-x86_64.tar.gz --repo BolvicBolvicovic/mogbus
```

`gh attestation verify` proves the file was built by this repository's release workflow from the
tagged commit. The SBOM is the `.spdx.json` file (SPDX 2.3): mogbus (MIT, tarball checksum), the
`bin/mogbus` file with SHA-1/SHA-256, vendored s7 (version from `third_party/s7/s7.h`, 0BSD) and
vlib (the submodule commit as version, no declared license), and each `DT_NEEDED` library of the binary (glibc and, in a TLS
build, OpenSSL with the version found at build time) as `DYNAMIC_LINK` relationships. Its timestamp is
`SOURCE_DATE_EPOCH`, so the same inputs give the same bytes.

## Reproducing a release

Check out the tag, install the same toolchain (clang and OpenSSL development files; compiler,
linker and OpenSSL versions change the bytes) and run:

```bash
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) make dist
sha256sum -c dist/SHA256SUMS      # compare with the published SHA256SUMS
```

`tools/check_reproducible.sh [make args]` builds `mogbus`, both libraries, the tarball and the SBOM
twice in two temporary copies of the tree (different absolute paths) and fails with a diff of
sha256 sums if anything differs; CI runs it in the `reproducible` job.

## Documentation site

The CI `docs` job builds the site (`make docs`). On pushes to `main`, the `pages` job deploys
`build/docs` to GitHub Pages at https://bolvicbolvicovic.github.io/mogbus/ (no `gh-pages` branch,
nothing generated is committed). One-time setup: repository Settings, Pages, Source: "GitHub Actions".

## Rules that keep builds reproducible

- Compile with `-ffile-prefix-map=$(CURDIR)=.` (`REPRO_FLAGS`, also for s7 and the `.so` link);
  no `__DATE__`, `__TIME__` or clock-derived data in any artifact.
- Source discovery is sorted (`find ... | sort`, `$(sort $(wildcard ...))`); archives use `ar rcsD`.
- The git revision goes only into `src/main.c`; ccache stays valid (it hashes the prefix map).
- Tarball: `tar --sort=name --mtime=@$SOURCE_DATE_EPOCH --owner=0 --group=0 --numeric-owner
  --format=gnu`, `gzip -n`, normalized modes.
