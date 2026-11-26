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
4. Dry run: start the workflow manually (`workflow_dispatch`); it builds and uploads workflow
   artifacts only and publishes nothing.

Releases are x86_64 only for now: the vendored test framework (`vlib/vtest.h`) uses x86
intrinsics, so the tests cannot run on aarch64 and untested binaries are not published. Release
assets per architecture: `mogbus-X.Y.Z-linux-<arch>.tar.gz` and
`mogbus-X.Y.Z-linux-<arch>.spdx.json`, plus one `SHA256SUMS`.

## Tarball layout

`make dist` (prod build only; `BUILD=dev`, `SAN=1` and `WITH_CONFIG=0` are refused) writes
`dist/mogbus-<version>-linux-<arch>.tar.gz`, `dist/<same>.spdx.json` and `dist/SHA256SUMS`
(`<arch>` is `uname -m`). The tarball has one top directory `mogbus-<version>-linux-<arch>/` with
`bin/mogbus`, `lib/libmogbus.a`, `lib/libmogbus.so`, `include/mogbus.h`,
`share/doc/mogbus/` (README.md, LICENSE, NOTICE.md, `docs/*.md`) and `share/mogbus/profiles/*.scm`.
`make clean` removes `dist/`.

The binary links OpenSSL dynamically (`libssl.so.3`, `libcrypto.so.3`, glibc): install `libssl3`
(Debian/Ubuntu) on the target. Use a `WITH_TLS=0` source build for a binary without OpenSSL.

`mogbus --version` prints `mogbus X.Y.Z (<git describe>)`; the revision is passed only to `main.c`
and is omitted outside a git checkout (`GIT_REVISION=` also disables it). A tagged commit gives
`vX.Y.Z`, a dirty tree appends `-dirty`.

## Verifying a download

```bash
sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify mogbus-X.Y.Z-linux-x86_64.tar.gz --repo BolvicBolvicovic/mogbus
```

`gh attestation verify` proves the file was built by this repository's release workflow from the
tagged commit. The SBOM is the `.spdx.json` file (SPDX 2.3): mogbus (MIT, tarball checksum), the
`bin/mogbus` file with SHA-1/SHA-256, vendored s7 (version from `third_party/s7/s7.h`, 0BSD) and
vlib (no declared license or version), and each `DT_NEEDED` library of the binary (glibc and, in a TLS
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

## Rules that keep builds reproducible

- Compile with `-ffile-prefix-map=$(CURDIR)=.` (`REPRO_FLAGS`, also for s7 and the `.so` link);
  no `__DATE__`, `__TIME__` or clock-derived data in any artifact.
- Source discovery is sorted (`find ... | sort`, `$(sort $(wildcard ...))`); archives use `ar rcsD`.
- The git revision goes only into `src/main.c`; ccache stays valid (it hashes the prefix map).
- Tarball: `tar --sort=name --mtime=@$SOURCE_DATE_EPOCH --owner=0 --group=0 --numeric-owner
  --format=gnu`, `gzip -n`, normalized modes.
