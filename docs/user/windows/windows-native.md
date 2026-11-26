# Native Windows build (experimental)

A cross-compiled `mogbus.exe` that runs the network modes without WSL2 or Docker. It is not released yet.

`make PLATFORM=win32 WITH_TLS=0` cross-compiles a native Windows build from Linux with MinGW-w64
(`x86_64-w64-mingw32-gcc`, `x86_64-w64-mingw32-ar`; `CROSS=` selects another toolchain prefix). It
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
| Python package | no |

Ctrl-C, Ctrl-Break and closing the console stop the server cleanly. Serial ports and TLS are the
remaining work; until then use [WSL2](windows-wsl2.md) for serial adapters. UDP behaves like on Linux:
a datagram longer than the protocol allows is dropped and the server keeps serving, and an ICMP port
unreachable from a vanished client does not disturb it.

## How it is tested

`tools/win32_check.sh` (CI job `windows-cross`) builds everything, runs the unit tests and starts
`mogbus.exe` in every network mode under Wine, checking it with the Linux `mogbus --health`,
`tools/modbus_probe.py` and the pymodbus interop script. The tests that fork, use PTYs or call POSIX
sockets directly stay Linux-only (`TESTS_POSIX_ONLY` in the Makefile). It has not been run on Windows
itself yet.

## See also

- [Running mogbus on Windows](windows-route.md)
- [Build from source](../building.md)
