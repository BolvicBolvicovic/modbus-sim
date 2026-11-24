# modbus-sim

A Modbus TCP/RTU/ASCII device simulator. You describe a device in a small text file; `modbus-sim`
pretends to be that device on the network or on a serial byte stream. Point your SCADA, PLC,
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
make                                   # builds ./modbus-sim (TLS enabled when OpenSSL headers are found)
./modbus-sim profiles/minimal.scm      # start the simulator on port 1502

# in another terminal:
./tools/modbus_probe.py holding 0 2    # -> [42, <a value that moves>]
```

Use `make -j$(nproc)` for a faster clean build; compilation is safe to parallelize.

Check a config without starting the server:

```bash
./modbus-sim --check profiles/power-meter.scm
```

Run the tests: `make tests`. Independent suites run in parallel (3 workers by default; set
`RUN_TESTS_JOBS=N` to change, capped by the number of suites); tests inside a suite stay sequential.
Run them under AddressSanitizer and UBSan: `make asan` (the vendored s7 interpreter is excluded
from instrumentation). The suite includes network integration tests that start the real server on
loopback; set `MODBUS_SIM_BIN` to test a different binary.

### Build options

The default build uses `cc` and the system linker with hardening flags
`-pie -Wl,-z,relro,-z,now`. If `clang`, `ccache` and `mold` are all on `PATH`, they are used as an
optional speed-up (`CC='ccache clang'` and `-fuse-ld=mold`), matching CI; if any is missing, the
build falls back to `cc` and the default linker. An explicit `CC` or `LDFLAGS` from the environment
or command line wins (for example, `CC=gcc make`); hardening flags are still appended unless
already present.

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
| `make WITH_TLS=0` | No OpenSSL cflags or libraries: builds plaintext TCP, RTU and ASCII transports only; TLS tests are skipped |

A no-TLS build rejects `(tls ...)` at config load time with `built without TLS support`, including
in `--check` mode. Serial, RTU, ASCII and plaintext TCP configs behave identically in both builds.
`make WITH_TLS=0 tests` runs the no-TLS test set; `make WITH_TLS=0 asan` and `make WITH_TLS=0 CC=clang`
work as expected. Documentation examples containing `(tls ...)` are skipped by `check_doc_examples.sh`
when checking a no-TLS binary.

## What it does today

| Feature | Details |
|---|---|
| Protocol | Modbus TCP (optionally TLS-wrapped), Modbus RTU (PTY/device or TCP byte stream), and Modbus ASCII (PTY/device or TCP byte stream). Function codes 01, 02, 03, 04, 05, 06, 07, 08 (public diagnostics subfunctions 0x0000-0x0004, 0x000A-0x0012, 0x0014), 11, 12, 15, 16, 17, 20, 21, 22, 23, 24, 43 (MEI 14). Exceptions 01, 02, 03, 0B. |
| Data | Coils, discrete inputs, input registers, holding registers. 16-bit, 32-bit and float32 values. |
| Behaviours | **static** (fixed value) and **ramp** (moves over time: wrap, bounce or hold). |
| Fault | One fault per device: **delay** responses or **drop** them, with a chance in percent. |
| Devices | Several devices on one port, told apart by unit id. |
| Clients | Up to 32 at the same time. A delayed response never blocks other clients. |
| Writes | Clients can write coils and holding registers. Writing a ramp freezes it at the written value. |

## Ready-made profiles

| File | Device | Shows |
|---|---|---|
| [`profiles/minimal.scm`](profiles/minimal.scm) | Two registers | The smallest config |
| [`profiles/power-meter.scm`](profiles/power-meter.scm) | Three-phase power meter | float32 values, ramps, energy counter |
| [`profiles/temperature-controller.scm`](profiles/temperature-controller.scm) | Temperature controller | signed values, setpoint, alarms |
| [`profiles/vfd-drive.scm`](profiles/vfd-drive.scm) | Motor drive | start-up ramp that holds, 32-bit counter |
| [`profiles/flow-meter.scm`](profiles/flow-meter.scm) | Flow meter | float32 and 32-bit totalizer |
| [`profiles/io-module.scm`](profiles/io-module.scm) | Remote I/O block | `(count N)` for many points |
| [`profiles/slow-sensor.scm`](profiles/slow-sensor.scm) | Sensor with late answers | delay fault |
| [`profiles/unreliable-meter.scm`](profiles/unreliable-meter.scm) | Meter that goes silent | drop fault |
| [`profiles/two-devices.scm`](profiles/two-devices.scm) | Two devices, one port | unit ids |

Each profile starts with a comment listing its register map. Copy one, change what you need.
See [docs/profiles.md](docs/profiles.md).

## Command line

```
modbus-sim [options] CONFIG.scm

  -c, --check          check the config, print a summary and exit
  -v, --verbose        log every request
  -h, --help           show this help
```

There are no server CLI options: the transport mode, path and all server settings are selected in
`(server ...)` in the config file. Port 502 is the Modbus standard but needs root on Linux; the
plaintext TCP default here is **1502**, or **802** when `(tls ...)` is enabled.

## Transports

`(server (mode NAME))` selects one of seven modes:

| Mode | Transport | Server entries allowed with `mode` |
|---|---|---|
| `tcp` | Modbus TCP (MBAP) over TCP (IPv4 or IPv6 literal bind), optionally TLS-wrapped | `port`, `bind`, `seed`, `idle-timeout-ms`, `tls` |
| `rtu-pty` | Modbus RTU over a new Linux PTY (slave path printed at startup) | `seed`, `baud`, `frame-gap-ms`, `strict-t15`, `broadcast-execute`, `parity`, `stop-bits` |
| `rtu-device` | Modbus RTU over an existing byte-stream device | `path`, plus the `rtu-pty` entries |
| `rtu-tcp` | Modbus RTU frames over a TCP listen socket (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `idle-timeout-ms`, `baud`, `frame-gap-ms`, `broadcast-execute` |
| `ascii-pty` | Modbus ASCII over a new Linux PTY (slave path printed at startup) | `seed`, `baud`, `data-bits`, `parity`, `stop-bits` |
| `ascii-device` | Modbus ASCII over an existing byte-stream device | `path`, plus the `ascii-pty` entries |
| `ascii-tcp` | Raw Modbus ASCII frames over a TCP listen socket (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `idle-timeout-ms` |

`mode` itself is allowed in every mode. Device modes require `(path "PATH")`; other modes reject it.
Any other entry is rejected at load time, even when its value is the default. See
[docs/config-reference.md](docs/config-reference.md) for ranges and defaults.

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

RTU frames are `[addr][PDU][CRC lo hi]`; bad CRC and unknown unit produce no response. Broadcast
(unit 0) requests are ignored by default. `broadcast-execute` (allowed in `rtu-pty`, `rtu-device`
and `rtu-tcp`) makes them execute on every configured device and never send a response. ASCII modes
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
streams only; it is never applied to RTU-over-TCP.

ASCII frames are `:` + uppercase hex for `[addr][PDU][LRC]` + `CR LF`; received hex may be upper- or
lowercase. Frames are delimited by `CR LF`, so RTU timing options do not apply; complete frames are
answered immediately, including several frames in one read. A missing colon, invalid hex, odd hex
count, bad LRC, or wrong terminator produces no response and never reaches the PDU handlers. Like
RTU, broadcast (unit 0), unknown units, and fault-dropped answers are silent. `ascii-tcp` carries
the same frames on TCP byte streams (up to 32 clients, with `idle-timeout-ms`); it is not Modbus TCP
(MBAP). `(baud N)` may set the serial line speed but never changes ASCII framing or timing, and is
rejected in `ascii-tcp`. FC 08 subfunction `0x0003` (Change ASCII Delimiter) is rejected with
exception 03; `CR LF` framing is fixed.

## Documentation

- [docs/config-reference.md](docs/config-reference.md) - every config entry, with examples and error help
- [docs/profiles.md](docs/profiles.md) - the profiles, and how to write your own
- [docs/modbus-support.md](docs/modbus-support.md) - what the server answers, exceptions, limits
- [docs/architecture.md](docs/architecture.md) - code layout, coding rules, roadmap

## Project layout

```
src/            the program, one folder per module (headers next to their .c files)
  build_config.h  WITH_TLS compile-time fallback shared by sources and tests
  config/       reads the Scheme file, checks it, builds devices
  device/       data tables, points, ramps, value encoding, faults
  modbus/       Modbus TCP MBAP, RTU CRC/framing, ASCII hex/LRC framing, and PDU handlers
  net/          TCP, RTU byte-stream, ASCII byte-stream, RTU-over-TCP, and ASCII-over-TCP network loops; OpenSSL TLS wrapper or no-TLS stub
  util/         clock, random numbers, logging
profiles/       ready-made device profiles
tests/          unit tests and integration tests (vtest)
tools/          modbus_probe.py, check_doc_examples.sh, run_tests.sh
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
