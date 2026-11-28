# Native Windows build (experimental)

A native `mogbus.exe` that runs the network modes without WSL2 or Docker, and `mogbus.dll` for embedding it from C or Python. It is experimental: the network modes are tested on Windows, serial ports and TLS are not available yet.

## Download

The release assets include `mogbus-<version>-windows-x86_64.zip` (with a `.spdx.json` SBOM and an entry in `SHA256SUMS`). It unpacks to one directory:

| Path | Content |
|---|---|
| `bin/mogbus.exe`, `bin/mogbus.dll` | the simulator (static, no MinGW DLLs needed) and the shared library |
| `lib/libmogbus.dll.a`, `lib/libmogbus.a` | import library for `mogbus.dll` and the static library (MinGW-w64) |
| `include/mogbus.h` | the C API |
| `profiles/`, `docs/` | example configs and these pages |
| `README.md`, `LICENSE`, `NOTICE.md` | |

The Python package finds `mogbus.dll` next to its sources or through `MOGBUS_LIB`.

`make PLATFORM=win32 WITH_TLS=0` builds it with MinGW-w64, cross-compiling from Linux
(`x86_64-w64-mingw32-gcc`, `x86_64-w64-mingw32-ar`; `CROSS=` selects another toolchain prefix, and
`CROSS=` empty uses the plain `gcc` and `ar` of an MSYS2 MinGW shell on Windows). It
produces `mogbus.exe` (statically linked, it needs no MinGW DLLs), `libmogbus.win32.a`, `mogbus.dll`
with its import library `libmogbus.dll.a`, and `mogbus-tests.exe`. The programs use Winsock 2 and need
Windows 10 or later (or Wine).

## What works

| | Native Windows build |
|---|---|
| `tcp`, `udp`, `rtu-tcp`, `rtu-udp`, `ascii-tcp`, `ascii-udp` | yes |
| `(http ...)` endpoint, `--health`, config checks, the C library | yes |
| TLS (`(tls ...)`) | no: build with `WITH_TLS=0`; configs with `(tls ...)` are rejected |
| Serial ports (`rtu-device`, `ascii-device`) | no: they fail with `serial ports are not supported on Windows yet` |
| PTY modes (`rtu-pty`, `ascii-pty`) | no: PTYs do not exist on Windows |
| `rtu-fd`, `ascii-fd` (embedding) | no: rejected with `fd mode is not supported on this platform` |
| Python package | yes, against `mogbus.dll` |

Ctrl-C, Ctrl-Break and closing the console stop the server cleanly. So does signalling the named event `Local\mogbus-stop-PID` (`PID` is the process id; `OpenEvent` and `SetEvent`), which works from any process of the same session, for example a service wrapper or a test harness, without sharing a console. Serial ports and TLS are the
remaining work; until then use [WSL2](windows-wsl2.md) for serial adapters. UDP behaves like on Linux:
a datagram longer than the protocol allows is dropped and the server keeps serving, and an ICMP port
unreachable from a vanished client does not disturb it.

## How it is tested

The unit and integration tests run natively on Windows (every network mode, the command line, the
Python package and the pymodbus interoperability script) and again under Wine on Linux. Only the tests
that need a PTY or termios stay Linux-only.

## See also

- [Running mogbus on Windows](windows-route.md)
- [Build from source](../building.md)
