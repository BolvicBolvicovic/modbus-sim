# modbus-sim

A Modbus TCP/RTU device simulator. You describe a device in a small text file; `modbus-sim`
pretends to be that device on the network. Point your SCADA, PLC, gateway or test script at it.

You do **not** need to know Scheme. The config file is a list of `(name value)` entries, and
there are ready-made profiles you can copy.

```scheme
(server (port 1502))

(device
  (name "Minimal example")
  (unit-id 1)
  (holding 0 (static 42))                                                      ; never changes
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap))))  ; counts up
```

## Quick start

```bash
make                                   # builds ./modbus-sim (needs gcc and make)
./modbus-sim profiles/minimal.scm      # start the simulator on port 1502

# in another terminal:
./tools/modbus_probe.py holding 0 2    # -> [42, <a value that moves>]
```

Check a config without starting the server:

```bash
./modbus-sim --check profiles/power-meter.scm
```

Run the tests: `make tests`. Independent suites run in parallel (3 workers by default; set
`RUN_TESTS_JOBS=N` to change, capped by the number of suites); tests inside a suite stay sequential.
Run them under AddressSanitizer and UBSan: `make asan` (the vendored s7 interpreter is excluded
from instrumentation). The suite includes network integration tests that start the real server on
loopback; set `MODBUS_SIM_BIN` to test a different binary.

## What it does today

| Feature | Details |
|---|---|
| Protocol | Modbus TCP and Modbus RTU (PTY/device byte stream). Function codes 01, 02, 03, 04, 05, 06, 07, 08, 11, 12, 15, 16, 17, 20, 21, 22, 23, 24, 43 (MEI 14). Exceptions 01, 02, 03, 0B. |
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

  -p, --port N         TCP/RTU-over-TCP: listen on port N (overrides the config)
  -b, --bind ADDR      TCP/RTU-over-TCP: listen on IPv4 address ADDR (overrides the config)
      --rtu-pty        RTU: create a PTY and print its slave path
      --rtu-device PATH RTU: serve RTU over an existing serial-like device
      --rtu-tcp        RTU-over-TCP: serve RTU frames over TCP
      --baud N         RTU/RTU-over-TCP: baud for t3.5 timing (overrides config/tty)
      --frame-gap-ms N RTU/RTU-over-TCP: explicit inter-frame silence override in ms
      --strict-t15     RTU byte stream: opt-in t1.5 discard heuristic (never RTU-over-TCP)
      --parity NAME    RTU byte stream: none|even|odd|mark|space (overrides config)
      --stop-bits N    RTU byte stream: 1 or 2 stop bits (overrides config)
  -c, --check          check the config, print a summary and exit
  -v, --verbose        log every request
  -h, --help           show help
```

Port 502 is the Modbus standard but needs root on Linux. The default here is **1502**.

In RTU mode, `--rtu-pty` creates a Linux PTY and prints `RTU PTY slave: /dev/pts/N`; connect your
RTU client to that path. `--rtu-device PATH` serves RTU on an existing bidirectional byte stream.
RTU frames are `[addr][PDU][CRC lo hi]`; bad CRC, unknown unit, and broadcast (unit 0) produce no
response. Broadcast requests are not executed. `--rtu-device` opens with `O_NONBLOCK` so startup does not
hang on FIFOs or similar paths; when the device is a tty, `modbus-sim` configures it as raw mode with
the selected parity and stop bits (default 8N1) and preserves the existing baud rate unless
`--baud`/config `(baud N)` is provided. Use `--parity none|even|odd|mark|space` or config
`(server (parity NAME))`, and `--stop-bits 1|2` or `(server (stop-bits N))`, to change the tty line
format; `mark` and `space` need `CMSPAR` support. These settings apply only to byte-stream RTU.
Unit ids 0 and 248-255 are rejected in RTU mode because 0 is broadcast and 248-255 are reserved.

RTU frame boundaries use Modbus t3.5 when a baud rate is known (explicit `--baud`/config `(baud N)`,
or the tty's existing baud rate). `--frame-gap-ms` is an explicit inter-frame silence override and
takes precedence when set. For PTY/pipe streams with no known baud and no override, the default 20 ms
fallback is used; read() boundaries do not expose true UART character timing.

`--strict-t15` (or config `(server (strict-t15 on))`) enables an opt-in best-effort t1.5 discard
heuristic for RTU byte streams in baud-derived auto mode. It is never applied to RTU-over-TCP.

`--rtu-tcp` serves Modbus RTU frames (`[addr][PDU][CRC lo hi]`) on a TCP listen socket as a
separate transport/framing choice. It is not Modbus TCP (MBAP) and does not change existing TCP or
RTU byte-stream behavior. `--port`/`--bind` apply to TCP and RTU-over-TCP; `--baud`/`--frame-gap-ms`
apply to RTU and RTU-over-TCP for RTU frame timing. `--parity`/`--stop-bits` apply only to RTU byte
streams; non-default config values are rejected with `--check --rtu-tcp`, while plain TCP ignores
them.

## Documentation

- [docs/config-reference.md](docs/config-reference.md) - every config entry, with examples and error help
- [docs/profiles.md](docs/profiles.md) - the profiles, and how to write your own
- [docs/modbus-support.md](docs/modbus-support.md) - what the server answers, exceptions, limits
- [docs/architecture.md](docs/architecture.md) - code layout, coding rules, roadmap

## Project layout

```
src/            the program, one folder per module (headers next to their .c files)
  config/       reads the Scheme file, checks it, builds devices
  device/       data tables, points, ramps, value encoding, faults
  modbus/       Modbus TCP MBAP, RTU CRC/framing, and PDU handlers
  net/          TCP, RTU byte-stream, and RTU-over-TCP network loops
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
