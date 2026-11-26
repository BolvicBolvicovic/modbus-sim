# Windows troubleshooting

The usual reasons mogbus is not reachable from Windows, and the sources of the networking advice.

- **Port already in use.** mogbus fails to bind when another program already uses the port. In NAT
  mode the WSL port is separate from Windows ports; in mirrored mode and with Docker Desktop the
  Windows port is the one that counts. Find the owner on Windows with `netstat -ano | findstr :1502`
  and choose another `(port ...)`.
- **Firewall prompts.** The first time a program listens on a non-loopback address Windows may ask to
  allow it. For Docker Desktop allow `com.docker.backend.exe`. Without the permission, other machines
  time out while `localhost` still works.
- **Works inside WSL but not from Windows.** Check `bind`: `127.0.0.1` in NAT mode is only reachable
  through localhost forwarding (TCP). For the WSL IP address or for UDP, use `0.0.0.0` or mirrored
  mode.
- **It worked yesterday.** In NAT mode the WSL IP address can change when WSL restarts. Query it again
  with `wsl hostname -I` and update the `portproxy` rule or your client.
- **Check that mogbus answers.** Inside WSL, `./mogbus --health CONFIG.scm` sends one real Modbus
  request to the configured server and exits 0 when it answers (see [Health check](../deployment/health-check.md)).
  From Windows, `tools/modbus_probe.py` tests the full path, and with `(http (port 9102))` in the
  config `curl http://localhost:9102/healthz` tests HTTP, see
  [Health, readiness and metrics endpoint](../observability/http-endpoint.md).

## Sources

- [Accessing network applications with WSL](https://learn.microsoft.com/en-us/windows/wsl/networking) (NAT mode, localhost, LAN access, `portproxy`, mirrored mode, Hyper-V firewall)
- [Advanced settings configuration in WSL](https://learn.microsoft.com/en-us/windows/wsl/wsl-config) (`.wslconfig`, `networkingMode`, `localhostForwarding`)
- [Connect USB devices](https://learn.microsoft.com/en-us/windows/wsl/connect-usb) (`usbipd-win`)
- [netsh interface portproxy](https://learn.microsoft.com/en-us/windows-server/networking/technologies/netsh/netsh-interface-portproxy)
- [Docker Desktop networking](https://docs.docker.com/desktop/features/networking/) and [how-tos](https://docs.docker.com/desktop/features/networking/networking-how-tos/) (`host.docker.internal`, published ports)
- [Docker port publishing](https://docs.docker.com/engine/network/port-publishing/) (`-p`, `/udp`, host address)
- [com0com and hub4com](https://sourceforge.net/projects/com0com/)

## See also

- [Reaching mogbus from Windows](windows-networking.md)
- [Running mogbus on Windows](windows-route.md)
