# mogbus

A Modbus TCP/UDP/RTU/ASCII device simulator. You describe a device in a small text file; `mogbus`
pretends to be that device on the network (TCP or UDP) or on a serial byte stream. Point your SCADA, PLC,
gateway or test script at it.

You do **not** need to know Scheme. The config file is a list of `(name value)` entries, and
there are ready-made profiles you can copy.

```scheme
(server (mode tcp) (port 1502))

(device
  (name "Minimal example")
  (unit-id 1)
  (holding 0 (static 42))                                                      ; never changes
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap))))  ; counts up
```

## Quick start

```bash
make                                   # builds ./mogbus (TLS enabled when OpenSSL headers are found)
./mogbus profiles/minimal.scm          # start the simulator on port 1502

# in another terminal:
./tools/modbus_probe.py holding 0 2    # -> [42, <a value that moves>]
```

`tools/modbus_probe.py` is a small Modbus TCP (MBAP) client; the RTU, ASCII and UDP modes need a
client that speaks their framing.

Use `make -j$(nproc)` for a faster clean build; compilation is safe to parallelize.

Check a config without starting the server:

```bash
./mogbus --check profiles/power-meter.scm
```

`--check` prints the same summary as a normal start (transport, devices, point counts, faults) and
validates the server settings, including TLS files, without opening any socket or device.

Run the tests: `make tests`. Every test runs in its own process, one per CPU in parallel (set
`RUN_TESTS_JOBS=N` to change), slowest first: durations of the last run are kept in
`mogbus-tests.times` / `mogbus-asan-tests.times` (ignored by git, removed by `make clean`).
Run them under AddressSanitizer and UBSan: `make asan` (the vendored s7 interpreter is excluded
from instrumentation). The suite includes network integration tests that start the real server on
loopback; set `MOGBUS_BIN` to test a different binary.

### Build options

The default build prefers `clang`, falling back to `gcc` and then `cc`, and uses the system linker
with hardening flags `-pie -Wl,-z,relro,-z,now`. `ccache` and `mold` are independent optional
speed-ups: `ccache` prefixes the chosen compiler when installed, and `-fuse-ld=mold` is used only
when mold is installed and the compiler accepts it (as in CI, where all three are present). An
explicit `CC` or `LDFLAGS` from the environment or command line wins (for example, `CC=gcc make`);
hardening flags are still appended unless already present.

`BUILD` controls optimization (`prod` by default, matching CI):

| Value | Effect |
|---|---|
| `make` / `make BUILD=prod` | `-O2` with `_FORTIFY_SOURCE=2`; the hardened production build |
| `make BUILD=dev` | `-O0` without `_FORTIFY_SOURCE`; faster rebuilds, with separate object files and automatic relinking when switching modes |

The vendored s7 config parser is always built at `-O0`: it only runs at startup and dominates compile
time. `CFLAGS` still overrides the complete flag set if you need a custom build.

For the fastest local iteration, use `BUILD=dev` (or its `make dev` shorthand), which keeps the
default toolchain and uses separate `-O0` objects:

```bash
make -j$(nproc) dev          # BUILD=dev
make -j$(nproc) dev tests    # BUILD=dev, plus the test suite
```

`WITH_TLS` controls the OpenSSL dependency (`auto` by default):

| Value | Effect |
|---|---|
| `make` / `make WITH_TLS=auto` | TLS enabled only when `pkg-config --exists openssl` succeeds; behavior is unchanged on systems with OpenSSL development files |
| `make WITH_TLS=1` | Require TLS; fail immediately with a clear error if `pkg-config` or OpenSSL development files are missing |
| `make WITH_TLS=0` | No OpenSSL cflags or libraries: builds every transport except TLS (plaintext TCP, UDP, RTU and ASCII); TLS tests are skipped |

A no-TLS build rejects `(tls ...)` at config load time with `built without TLS support`, including
in `--check` mode. Serial, RTU, ASCII, UDP and plaintext TCP configs behave identically in both builds.
`make WITH_TLS=0 tests` runs the no-TLS test set; `make WITH_TLS=0 asan` and `make WITH_TLS=0 CC=clang`
work as expected. Documentation examples containing `(tls ...)` are skipped by `check_doc_examples.sh`
when checking a no-TLS binary.

`WITH_CONFIG=1|0` (default `1`) controls the Scheme config loader. `make WITH_CONFIG=0` compiles without
`src/config`, `src/server/server_config.c` and the vendored s7, and builds only `libmogbus.a`/`libmogbus.so`
(there is no `mogbus` binary); the config functions of the C API then fail with `built without config
support`. `make WITH_CONFIG=0 tests` runs the tests that do not need config or the binary. See
[docs/library.md](docs/library.md).

`make dist` builds the prod configuration and writes a reproducible release tarball
`dist/mogbus-<version>-linux-<arch>.tar.gz` (binary, both libraries, header, docs, profiles), an
SPDX SBOM and `dist/SHA256SUMS`. Builds are bit-for-bit reproducible across directories
(`tools/check_reproducible.sh`). Tagged releases are built and attested by GitHub Actions; see
[docs/release.md](docs/release.md).

## What it does today

| Feature | Details |
|---|---|
| Protocol | Modbus TCP (optionally TLS-wrapped) and Modbus UDP; Modbus RTU and Modbus ASCII over a PTY/serial device, a TCP byte stream, or UDP datagrams (one frame per datagram). Function codes 01, 02, 03, 04, 05, 06, 07, 08 (public diagnostics subfunctions 0x0000-0x0004, 0x000A-0x0012, 0x0014), 11, 12, 15, 16, 17, 20, 21, 22, 23, 24, 43 (MEI 14 and MEI 13 CANopen SDO upload/download). Exceptions 01, 02, 03, 0B; fault injection adds 04-08 and 0A. |
| Data | Coils, discrete inputs, input registers, holding registers. 16-bit, 32-bit and float32 values. |
| Behaviours | **static** (fixed value), **ramp** (moves over time: wrap, bounce or hold), and **machine** (bounded scripted state machine, see [config reference](docs/config-reference.md#machine-)). |
| Fault | Repeatable per device, one definition per kind: **delay**, **drop**, **exception**, **corrupt** (PDU, register or checksum), **restart**, **sensor-disconnect**, and **disconnect** (TCP-family modes only), each firing by percent chance or a non-repeating `after-ms`/`count` schedule (max 8 steps). |
| Devices | Up to 16 devices on one port or line, told apart by unit id. |
| Clients | Up to 32 TCP connections at a time; UDP modes answer any number of peers on one socket. A delayed response never blocks other clients. |
| Writes | Clients can write coils and holding registers. Writing a ramp freezes it at the written value. |
| Embedding | `libmogbus.a` / `libmogbus.so` with a small C API (`include/mogbus.h`); the `mogbus` binary is itself a client of it. |

## Ready-made profiles

| File | Device | Shows |
|---|---|---|
| [`profiles/minimal.scm`](profiles/minimal.scm) | Two registers | The smallest config |
| [`profiles/modbus-udp.scm`](profiles/modbus-udp.scm) | Two registers over UDP | `(mode udp)` transport |
| [`profiles/ascii-udp.scm`](profiles/ascii-udp.scm) | Two registers, ASCII over UDP | `(mode ascii-udp)` transport |
| [`profiles/rtu-udp.scm`](profiles/rtu-udp.scm) | Two registers over RTU-over-UDP | `(mode rtu-udp)` transport |
| [`profiles/power-meter.scm`](profiles/power-meter.scm) | Three-phase power meter | float32 values, ramps, energy counter |
| [`profiles/temperature-controller.scm`](profiles/temperature-controller.scm) | Temperature controller | signed values, setpoint, alarms |
| [`profiles/vfd-drive.scm`](profiles/vfd-drive.scm) | Motor drive | start-up ramp that holds, 32-bit counter |
| [`profiles/motor.scm`](profiles/motor.scm) | Motor with a scripted machine | start/stop states, timed ramp, random trip |
| [`profiles/flow-meter.scm`](profiles/flow-meter.scm) | Flow meter | float32 and 32-bit totalizer |
| [`profiles/io-module.scm`](profiles/io-module.scm) | Remote I/O block | `(count N)` for many points |
| [`profiles/slow-sensor.scm`](profiles/slow-sensor.scm) | Sensor with late answers | delay fault |
| [`profiles/unreliable-meter.scm`](profiles/unreliable-meter.scm) | Meter that goes silent | drop fault |
| [`profiles/two-devices.scm`](profiles/two-devices.scm) | Two devices, one port | unit ids |

Each profile starts with a comment listing its register map. Copy one, change what you need.
See [docs/profiles.md](docs/profiles.md).

## Command line

```
mogbus [options] CONFIG.scm

  -c, --check          check the config, print a summary and exit
  -v, --verbose        log every request
  -V, --version        print the version (and git revision, when built from a checkout) and exit
  -h, --help           show this help
```

There are no server CLI options: the transport mode, path and all server settings are selected in
`(server ...)` in the config file. Exit status: 0 on success (or a clean stop on SIGINT/SIGTERM), 1 for
a config error, a serial/PTY open failure or a runtime poll error, 2 for invalid CLI arguments or a
listener (TCP/UDP bind) or TLS start-up failure. Port 502 is the Modbus standard but needs root on Linux; the
plaintext TCP default here is **1502**, or **802** when `(tls ...)` is enabled.

## Transports

`(server (mode NAME))` selects one of ten modes:

| Mode | Transport | Server entries allowed with `mode` |
|---|---|---|
| `tcp` | Modbus TCP (MBAP) over TCP (IPv4 or IPv6 literal bind), optionally TLS-wrapped | `port`, `bind`, `seed`, `idle-timeout-ms`, `tls` |
| `rtu-pty` | Modbus RTU over a new Linux PTY (slave path printed at startup) | `seed`, `baud`, `frame-gap-ms`, `strict-t15`, `broadcast-execute`, `parity`, `stop-bits` |
| `rtu-device` | Modbus RTU over an existing byte-stream device | `path`, plus the `rtu-pty` entries |
| `rtu-tcp` | Modbus RTU frames over a TCP listen socket (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `idle-timeout-ms`, `baud`, `frame-gap-ms`, `broadcast-execute` |
| `ascii-pty` | Modbus ASCII over a new Linux PTY (slave path printed at startup) | `seed`, `baud`, `data-bits`, `parity`, `stop-bits` |
| `ascii-device` | Modbus ASCII over an existing byte-stream device | `path`, plus the `ascii-pty` entries |
| `ascii-tcp` | Raw Modbus ASCII frames over a TCP listen socket (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `idle-timeout-ms` |
| `udp` | Modbus UDP: one MBAP ADU per datagram (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed` |
| `ascii-udp` | Modbus ASCII: one `:`...CR LF frame per UDP datagram (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed` |
| `rtu-udp` | Modbus RTU frames, one per UDP datagram (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `broadcast-execute` |

`mode` itself is allowed in every mode. Device modes require `(path "PATH")`; other modes reject it.
Any other entry is rejected at load time, even when its value is the default. See
[docs/config-reference.md](docs/config-reference.md) for ranges and defaults.

TCP-family listeners (`tcp`, `rtu-tcp`, `ascii-tcp`), `udp`, `ascii-udp` and `rtu-udp` bind to loopback (`127.0.0.1`) by default, so
they accept only local clients. Bind explicitly to `0.0.0.0` (or `::` for IPv6) to accept remote
connections.

```scheme
; Modbus TCP (plaintext)
(server (mode tcp) (port 1502) (bind "127.0.0.1") (idle-timeout-ms 5000))
```

TLS wraps the same MBAP frames on `tcp` listeners only, in builds with TLS support. Certificate and
private key paths are required; client certificates are required by default. The CA certificate is
required while client authentication is on. `(tls ...)` is rejected at load time in every other mode,
and in every mode of a no-TLS build.

```scheme
; Modbus TCP Security: TLS-wrapped MBAP, default port 802
; example PEM files from tests/tls; replace with your own paths
(server (mode tcp) (bind "127.0.0.1")
  (tls (certificate "tests/tls/server.pem")
       (private-key "tests/tls/server.key")
       (ca-certificate "tests/tls/ca.pem")))

(device (name "TLS device") (unit-id 1) (holding 0 (static 42)))
```

Set `(require-client-certificate off)` in `(tls ...)` to simulate anonymous clients; the CA entry is
then optional. This is a testing relaxation, not a recommended deployment setting. There is no
role-based authorization based on certificate fields. See
[docs/config-reference.md](docs/config-reference.md) for TLS options and defaults.

```scheme
; Modbus RTU: choose a PTY or an existing device, never both in one config
(server (mode rtu-pty) (baud 9600) (frame-gap-ms 20)
        (parity even) (stop-bits 2) (strict-t15 on))
(server (mode rtu-device) (path "/dev/ttyUSB0") (baud 19200))
```

```scheme
; Modbus RTU over TCP (RTU frames, not MBAP)
(server (mode rtu-tcp) (port 1502) (bind "0.0.0.0") (baud 9600) (broadcast-execute on))
```

```scheme
; Modbus ASCII: choose a PTY or an existing device, never both in one config
(server (mode ascii-pty) (baud 9600))
(server (mode ascii-device) (path "/dev/ttyUSB1"))
(server (mode ascii-pty) (data-bits 7) (parity even) (stop-bits 1)) ; 7E1 line
```

```scheme
; Modbus ASCII over TCP (raw ASCII frames, not MBAP)
(server (mode ascii-tcp) (port 1502) (bind "0.0.0.0") (idle-timeout-ms 5000))
```

```scheme
; UDP: one frame per datagram, replies go to the sender; no connections, no idle timeout, no TLS
(server (mode udp) (port 1502))        ; Modbus UDP (MBAP ADU)
(server (mode rtu-udp) (port 1502))    ; RTU frames, also accepts broadcast-execute
(server (mode ascii-udp) (port 1502))  ; ASCII :...CR LF frames
```

In the UDP modes a datagram must hold exactly one complete frame: there is no reassembly across
datagrams, and anything else (two frames, trailing bytes, a length or checksum mismatch) is ignored.
`disconnect` faults are rejected because there is no connection to close.

RTU frames are `[addr][PDU][CRC lo hi]`; bad CRC and unknown unit produce no response. Broadcast
(unit 0) requests are ignored by default. `broadcast-execute` (allowed in `rtu-pty`, `rtu-device`,
`rtu-tcp` and `rtu-udp`) makes them execute on every configured device and never send a response. ASCII modes
never execute broadcasts and reject `broadcast-execute` at load time. Unit ids 0 and 248-255 are
rejected in all RTU/ASCII modes because 0 is broadcast and 248-255 are reserved. MBAP/TCP unit 0 is
only a unit id and never broadcast.

Device modes open the path with `O_NONBLOCK` so startup does not hang on FIFOs or similar paths.
When the fd is a tty, RTU modes configure raw mode with the selected parity and stop bits (default
8N1) and preserve the existing baud rate unless `(baud N)` is given. Use
`(parity none|even|odd|mark|space)` and `(stop-bits 1|2)` to change the tty line format; `mark` and
`space` need `CMSPAR` support. ASCII ttys also default to raw 8N1; select the traditional Modbus
ASCII 7E1 line with `(data-bits 7) (parity even) (stop-bits 1)`.

RTU frame boundaries use t3.5 when baud is known (explicit `(baud N)` or the tty's existing baud).
`(frame-gap-ms N)` is an explicit inter-frame silence override and takes precedence when set. For
PTY/pipe streams with no known baud and no override, the default 20 ms fallback is used. In
baud-derived auto mode, `(strict-t15 on)` enables a best-effort t1.5 discard heuristic for RTU byte
streams only; it is never applied to RTU-over-TCP, and `rtu-udp` has no timing at all (the datagram
is the frame).

ASCII frames are `:` + uppercase hex for `[addr][PDU][LRC]` + `CR LF`; received hex may be upper- or
lowercase. Frames are delimited by `CR LF`, so RTU timing options do not apply; complete frames are
answered immediately, including several frames in one read. A missing colon, invalid hex, odd hex
count, bad LRC, or wrong terminator produces no response and never reaches the PDU handlers. Like
RTU, broadcast (unit 0), unknown units, and fault-suppressed answers are silent. `ascii-tcp` carries
the same frames on TCP byte streams (up to 32 clients, with `idle-timeout-ms`); it is not Modbus TCP
(MBAP). `(baud N)` may set the serial line speed but never changes ASCII framing or timing, and is
rejected in `ascii-tcp`. `ascii-udp` carries one frame per datagram with no resynchronization or
partial-frame timeout. FC 08 subfunction `0x0003` (Change ASCII Delimiter) is rejected with
exception 03; `CR LF` framing is fixed.

## Use as a library

mogbus also builds as `libmogbus.a` / `libmogbus.so` (`make lib`) with a small C API in
`include/mogbus.h`: create devices in code or from a config file, start any transport and step it
from your own loop. The `mogbus` binary is built the same way: `src/main.c` includes only
`mogbus.h` and links against the static library. `make WITH_CONFIG=0` builds a library without the
Scheme config loader for hosts that build devices in code. See [docs/library.md](docs/library.md)
and `examples/embed_tcp.c`.

## Documentation

- [docs/config-reference.md](docs/config-reference.md) - every config entry, with examples and error help
- [docs/profiles.md](docs/profiles.md) - the profiles, and how to write your own
- [docs/modbus-support.md](docs/modbus-support.md) - what the server answers, exceptions, limits
- [docs/library.md](docs/library.md) - embedding mogbus: build, link flags, API, lifetimes, threading
- [docs/release.md](docs/release.md) - cutting a release, reproducible builds, SBOM, verifying downloads
- [docs/architecture.md](docs/architecture.md) - code layout, coding rules, roadmap

## Project layout

```
include/        mogbus.h, the public C API of the library
examples/       embed_tcp.c, a host program using only mogbus.h
src/            the program, one folder per module (headers next to their .c files)
  build_config.h  WITH_TLS and WITH_CONFIG compile-time fallbacks shared by sources and tests
  config/       reads the Scheme file, checks it, builds devices
  device/       data tables, points, ramps, value encoding, faults
  modbus/       Modbus TCP MBAP, RTU CRC/framing, ASCII hex/LRC framing, and PDU handlers
  net/          TCP, RTU byte-stream, ASCII byte-stream, RTU-over-TCP, ASCII-over-TCP, UDP, ASCII-over-UDP and RTU-over-UDP network loops; OpenSSL TLS wrapper or no-TLS stub
  api/          the public C API implementation (top layer)
  server/       transport-independent server facade and the config adapter
  util/         arena implementation, clock, random numbers, logging
profiles/       ready-made device profiles
tests/          unit tests and integration tests (vtest)
tools/          modbus_probe.py, check_deps.sh, check_doc_examples.sh, check_reproducible.sh, check_toolchain.sh, gen_sbom.sh, run_tests.sh
third_party/s7  the s7 Scheme interpreter (used only to read the config)
vlib/           the vlib toolchain (arena allocator, test framework), unmodified
```

## Notes

- Built as plain C (gnu11) with the [vlib](https://github.com/BolvicBolvicovic/vlib) toolchain.
- All memory in this program comes from vlib arenas. The embedded s7 interpreter manages its own
  memory internally (it is only alive while the config is read).
- Linux only for now (uses `poll`, `accept4`, and vlib's `mmap` arenas).

## Contributing and security

- [CONTRIBUTING.md](CONTRIBUTING.md) - how to propose changes
- [SECURITY.md](SECURITY.md) - how to report security issues

## License

This project is licensed under the [MIT License](LICENSE). Third-party components are documented in
[NOTICE.md](NOTICE.md).
