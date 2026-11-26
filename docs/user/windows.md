# Running mogbus on Windows

mogbus is a Linux program. On Windows you run it in a Linux environment that Windows provides:
Docker Desktop or WSL2 (Windows Subsystem for Linux 2). An experimental native Windows build of the
network modes exists, see [Native build (experimental)](#native-build-experimental); it is not
released yet.

## Which route to use

| | Docker Desktop | WSL2 |
|---|---|---|
| Network modes (`tcp`, `udp`, `rtu-tcp`, ...) | yes | yes |
| Nothing to build | yes (build the image once from the `Dockerfile`) | no (build from source, or unpack the release tarball) |
| Serial devices (`rtu-device`, `ascii-device`) | no, Docker Desktop cannot pass Windows COM ports to containers | yes, with `usbipd-win` |
| Python package | no | yes, inside WSL |

Pick Docker Desktop when the simulator only has to answer over the network. Pick WSL2 when you need a
serial adapter, the Python package, or want to edit and rebuild mogbus.

## Native build (experimental)

`make PLATFORM=win32 WITH_TLS=0` cross-compiles a native Windows build from Linux with MinGW-w64
(`x86_64-w64-mingw32-gcc`, `x86_64-w64-mingw32-ar`; `CROSS=` selects another toolchain prefix). It
produces `mogbus.exe` (statically linked, it needs no MinGW DLLs), `libmogbus.win32.a`, `mogbus.dll`
with its import library `libmogbus.dll.a`, and `mogbus-tests.exe`. The programs use Winsock 2 and
need Windows 10 or later (or Wine).

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
remaining work; until then use WSL2 for serial adapters. UDP behaves like on Linux: a datagram longer
than the protocol allows is dropped and the server keeps serving, and an ICMP port unreachable from a
vanished client does not disturb it.

`tools/win32_check.sh` (CI job `windows-cross`) builds everything, runs the unit tests and starts
`mogbus.exe` in every network mode under Wine, checking it with the Linux `mogbus --health`,
`tools/modbus_probe.py` and the pymodbus interop script. The tests that fork, use PTYs or call
POSIX sockets directly stay Linux-only (`TESTS_POSIX_ONLY` in the Makefile). It has not been run on
Windows itself yet.

## Docker Desktop

Build and run the image as described in [deployment.md](deployment.md), from a PowerShell prompt in a
checkout of the repository:

```powershell
docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
docker run --rm -p 127.0.0.1:1502:1502 mogbus:0.0.1
```

The container config binds `0.0.0.0` inside the container, so the published port works. What you
publish with `-p` decides who can connect:

| Who connects | Option |
|---|---|
| Windows programs on the same PC | `-p 127.0.0.1:1502:1502`, then connect to `localhost:1502` |
| Other machines on the network | `-p 1502:1502` (no host address means all host interfaces); allow the connection in the Windows firewall |
| UDP (`mode udp`, `ascii-udp`, `rtu-udp`) | add the protocol: `-p 127.0.0.1:1502:1502/udp`; to publish both, repeat `-p` for `/tcp` and `/udp` |

For UDP use a config with `(server (mode udp) (port 1502) (bind "0.0.0.0"))`, mounted over
`/etc/mogbus/mogbus.scm` as shown in [deployment.md](deployment.md). The bind address inside the
container must be `0.0.0.0`, like in the default container config; `127.0.0.1` inside the container
cannot be reached through a published port.

Inbound traffic goes through the Docker Desktop backend process (`com.docker.backend.exe`), so a
Windows firewall rule or prompt applies to that program. Docker's documentation describes the `/udp`
syntax but says nothing specific about UDP on Docker Desktop for Windows, so test UDP with your own client.

A container reaches a program running on Windows through the name `host.docker.internal`. mogbus is a
server, so you rarely need this.

## WSL2

### Set up

In PowerShell as administrator, install a distribution and open it:

```powershell
wsl --install -d Ubuntu
```

In the Linux shell, install the build dependencies and build:

```bash
sudo apt update
sudo apt install make clang libssl-dev pkg-config python3 git
git clone --recurse-submodules https://github.com/BolvicBolvicovic/mogbus.git
cd mogbus
make
./mogbus profiles/minimal.scm
```

Without OpenSSL, `make WITH_TLS=0` builds everything except `(tls ...)`. Instead of building, you can
unpack a release tarball (`mogbus-<version>-linux-x86_64.tar.gz` from the GitHub releases) inside
WSL; it needs the OpenSSL runtime library (`libssl3` on Ubuntu 22.04 and later).

Work in the Linux file system (`~/mogbus`), not under `/mnt/c`: builds are much faster there.

The Python package works inside WSL as on any Linux machine, see [python.md](python.md):

```bash
make python
pip install ./python
```

## Networking

The default `bind` is `127.0.0.1`: only programs on the same machine can connect. A Windows program
and mogbus in WSL2 are not the same machine unless WSL localhost forwarding or mirrored mode makes them
appear so. Which `(bind ...)` to use depends on the case.

### WSL2 in the default NAT mode

WSL2 runs in a virtual machine with its own IP address behind a NAT.

- **TCP from Windows on the same PC.** Microsoft documents that an app in the Linux distribution can
  be reached from Windows with `localhost`, and the `.wslconfig` setting `localhostForwarding`
  (default `true`) controls it. Use the default `(bind "127.0.0.1")`; `localhost:1502` then works for
  `tcp`, `rtu-tcp` and `ascii-tcp`.
- **UDP from Windows on the same PC.** Do not rely on `localhost`. Microsoft's documentation shows
  localhost access for TCP-style apps only and makes no promise for UDP, and community reports say
  UDP is not forwarded in NAT mode. Use mirrored mode (below), or bind mogbus to `0.0.0.0` and send the
  datagrams to the WSL IP address.
- **Using the WSL IP address.** Connections to that address count as coming from the LAN, so the app must
  accept LAN connections: set `(bind "0.0.0.0")`. Get the address from Windows with
  `wsl hostname -I` (capital `I`).
- **Other machines on the network.** WSL2 is not reachable from the LAN by default. For TCP, forward a
  Windows port to the WSL address with `netsh` (PowerShell as administrator) and allow the port in the
  Windows firewall:

  ```powershell
  netsh interface portproxy add v4tov4 listenport=1502 listenaddress=0.0.0.0 connectport=1502 connectaddress=(wsl hostname -I)
  netsh interface portproxy delete v4tov4 listenport=1502 listenaddress=0.0.0.0
  ```

  The second command removes the rule again. `portproxy` only handles TCP (its syntax takes
  `protocol=tcp`), so it does not help for `udp`, `ascii-udp` or `rtu-udp`: use mirrored mode for those.
  mogbus needs `(bind "0.0.0.0")` here.
- **The WSL address can change** when the WSL VM restarts. Look it up again and update the `portproxy` rule
  (delete it and add it again).

### WSL2 in mirrored mode

Mirrored mode needs Windows 11 22H2 or later. It copies the Windows network interfaces into Linux.
Create or edit `%UserProfile%\.wslconfig` on Windows:

```ini
[wsl2]
networkingMode=mirrored
```

Then run `wsl --shutdown` and start WSL again. In this mode Microsoft documents that Windows and WSL
can reach each other through `localhost` (`127.0.0.1`; `::1` is not supported), and that you can connect
to WSL from your LAN. `localhostForwarding` is ignored.

- **Same PC:** the default `(bind "127.0.0.1")` is enough; connect to `127.0.0.1:1502` from Windows.
  Microsoft's pages do not describe UDP separately for mirrored mode; test it with your client.
- **Other machines:** use `(bind "0.0.0.0")` and connect to the Windows PC address. The Hyper-V firewall
  blocks inbound connections to WSL by default, so allow them in PowerShell as administrator, for example
  for UDP port 1502 (use `-Protocol TCP` for TCP):

  ```powershell
  New-NetFirewallHyperVRule -Name "mogbus-udp" -DisplayName "mogbus UDP 1502" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 1502
  ```

  To allow all inbound connections instead, use
  `Set-NetFirewallHyperVVMSetting -Name '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -DefaultInboundAction Allow`.
  The normal Windows firewall rules apply as well.

### Summary

| Case | WSL2 NAT | WSL2 mirrored | Docker Desktop |
|---|---|---|---|
| Windows program, TCP | `localhost`, `bind "127.0.0.1"` | `127.0.0.1`, `bind "127.0.0.1"` | `-p 127.0.0.1:1502:1502`, container binds `0.0.0.0` |
| Windows program, UDP | WSL IP + `bind "0.0.0.0"` | `127.0.0.1`, `bind "127.0.0.1"` (test it) | `-p 127.0.0.1:1502:1502/udp` |
| Other machines, TCP | `portproxy` + firewall + `bind "0.0.0.0"` | Hyper-V firewall rule + `bind "0.0.0.0"` | `-p 1502:1502` + firewall |
| Other machines, UDP | not possible with `portproxy`; use mirrored | Hyper-V firewall rule + `bind "0.0.0.0"` | `-p 1502:1502/udp` + firewall |

A server reachable by other machines has no authentication (except TLS with client certificates in
`tcp` mode). Open it only on networks you trust.

## Serial devices

Serial modes (`rtu-device`, `ascii-device`) run in WSL2. WSL2 has no access to Windows COM ports, but it
can use a USB serial adapter that you attach to it with
[usbipd-win](https://github.com/dorssel/usbipd-win). WSL needs a recent kernel (run `wsl --update` if
needed).

In PowerShell as administrator:

```powershell
winget install --interactive --exact dorssel.usbipd-win
usbipd list
usbipd bind --busid 4-4
usbipd attach --wsl --busid 4-4
```

Replace `4-4` with the bus ID of your adapter from `usbipd list`. Keep a WSL window open while you attach.
As long as the adapter is attached to WSL, Windows cannot use it; `usbipd detach --busid 4-4` gives it
back. The attach has to be repeated after the adapter is unplugged or WSL restarts.

In WSL the device appears as `/dev/ttyUSB0` (FTDI, CH340, CP210x) or `/dev/ttyACM0` (CDC). Your user needs
access to it:

```bash
ls -l /dev/ttyUSB0
sudo usermod -aG dialout $USER     # then open a new WSL shell
```

If the device does not appear, `lsusb` shows whether the adapter arrived; the WSL kernel may lack the
driver for it. Then point mogbus at it:

```scheme
(server
  (mode rtu-device)
  (path "/dev/ttyUSB0")
  (baud 19200)
  (parity even))

(device
  (name "Serial example")
  (unit-id 1)
  (holding 0 (static 42)))
```

Use `(mode ascii-device)` for Modbus ASCII. The serial options are in
[server keys](config/server/index.md).

Docker Desktop cannot pass Windows COM ports (or USB devices) into containers, so serial modes need WSL2.

### Windows software that only talks to a COM port

Some Windows tools can only open a COM port. When mogbus runs in WSL, bridge that COM port to a network
connection:

1. Run mogbus in `rtu-tcp` (or `ascii-tcp`) mode with `(bind "0.0.0.0")`, reachable from Windows as
   described above.
2. On Windows, create a virtual COM port with a third-party driver and connect it to the mogbus TCP port
   with a serial-to-TCP bridge. The open-source com0com (virtual COM port pairs) and hub4com (routes data
   between COM and TCP ports) are meant for this kind of setup, and commercial virtual serial port tools do
   the same. mogbus does not ship or test them; their setup is described in their own documentation.
3. Point the Windows tool at the virtual COM port.

The bridge carries the raw RTU or ASCII frames, which is what `rtu-tcp` and `ascii-tcp` speak, so mogbus
needs no serial hardware. The baud rate set in the tool has no effect on the TCP link, and timing-sensitive
RTU clients may see frames split differently than on a real wire.

## Troubleshooting

- **Port already in use.** mogbus fails to bind when another program already uses the port. In NAT mode
  the WSL port is separate from Windows ports; in mirrored mode and with Docker Desktop the Windows port
  is the one that counts. Find the owner on Windows with `netstat -ano | findstr :1502` and choose
  another `(port ...)`.
- **Firewall prompts.** The first time a program listens on a non-loopback address Windows may ask to
  allow it. For Docker Desktop allow `com.docker.backend.exe`. Without the permission, other machines
  time out while `localhost` still works.
- **Works inside WSL but not from Windows.** Check `bind`: `127.0.0.1` in NAT mode is only reachable
  through localhost forwarding (TCP). For the WSL IP address or for UDP, use `0.0.0.0` or mirrored mode.
- **It worked yesterday.** In NAT mode the WSL IP address can change when WSL restarts. Query it again
  with `wsl hostname -I` and update the `portproxy` rule or your client.
- **Check that mogbus answers.** Inside WSL, `./mogbus --health CONFIG.scm` sends one real Modbus request to
  the configured server and exits 0 when it answers (see [deployment.md](deployment.md)). From Windows,
  `tools/modbus_probe.py` tests the full path, and with `(http (port 9102))` in the config
  `curl http://localhost:9102/healthz` tests HTTP, see [observability.md](observability.md).

## Sources

- [Accessing network applications with WSL](https://learn.microsoft.com/en-us/windows/wsl/networking) (NAT mode, localhost, LAN access, `portproxy`, mirrored mode, Hyper-V firewall)
- [Advanced settings configuration in WSL](https://learn.microsoft.com/en-us/windows/wsl/wsl-config) (`.wslconfig`, `networkingMode`, `localhostForwarding`)
- [Connect USB devices](https://learn.microsoft.com/en-us/windows/wsl/connect-usb) (`usbipd-win`)
- [netsh interface portproxy](https://learn.microsoft.com/en-us/windows-server/networking/technologies/netsh/netsh-interface-portproxy)
- [Docker Desktop networking](https://docs.docker.com/desktop/features/networking/) and [how-tos](https://docs.docker.com/desktop/features/networking/networking-how-tos/) (`host.docker.internal`, published ports)
- [Docker port publishing](https://docs.docker.com/engine/network/port-publishing/) (`-p`, `/udp`, host address)
- [com0com and hub4com](https://sourceforge.net/projects/com0com/)
