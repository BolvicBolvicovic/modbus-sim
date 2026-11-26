# Running mogbus on Windows

mogbus is a Linux program. On Windows you run it in a Linux environment that Windows provides: Docker Desktop or WSL2.

Docker Desktop and WSL2 (Windows Subsystem for Linux 2) both run the normal Linux build. An
experimental [native Windows build](windows-native.md) of the network modes exists too; it is not
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

## Next steps

- [Docker Desktop](windows-docker.md): build the image, publish ports.
- [WSL2](windows-wsl2.md): install and build.
- [Reaching mogbus from Windows](windows-networking.md): which `bind` and which address to use.
- [Serial devices](windows-serial.md): USB serial adapters and COM-port-only tools.
- [Troubleshooting](windows-troubleshooting.md).
