# Reaching mogbus from Windows

Which `(bind ...)` to use, and which address to connect to, for WSL2 in NAT mode, WSL2 in mirrored mode and Docker Desktop.

The default `bind` is `127.0.0.1`: only programs on the same machine can connect. A Windows program
and mogbus in WSL2 are not the same machine unless WSL localhost forwarding or mirrored mode makes
them appear so.

## Summary

| Case | WSL2 NAT | WSL2 mirrored | Docker Desktop |
|---|---|---|---|
| Windows program, TCP | `localhost`, `bind "127.0.0.1"` | `127.0.0.1`, `bind "127.0.0.1"` | `-p 127.0.0.1:1502:1502`, container binds `0.0.0.0` |
| Windows program, UDP | WSL IP + `bind "0.0.0.0"` | `127.0.0.1`, `bind "127.0.0.1"` (test it) | `-p 127.0.0.1:1502:1502/udp` |
| Other machines, TCP | `portproxy` + firewall + `bind "0.0.0.0"` | Hyper-V firewall rule + `bind "0.0.0.0"` | `-p 1502:1502` + firewall |
| Other machines, UDP | not possible with `portproxy`; use mirrored | Hyper-V firewall rule + `bind "0.0.0.0"` | `-p 1502:1502/udp` + firewall |

A server reachable by other machines has no authentication (except TLS with client certificates in
`tcp` mode). Open it only on networks you trust.

## WSL2 in the default NAT mode

WSL2 runs in a virtual machine with its own IP address behind a NAT.

- **TCP from Windows on the same PC.** Microsoft documents that an app in the Linux distribution can
  be reached from Windows with `localhost`, and the `.wslconfig` setting `localhostForwarding`
  (default `true`) controls it. Use the default `(bind "127.0.0.1")`; `localhost:1502` then works for
  `tcp`, `rtu-tcp` and `ascii-tcp`.
- **UDP from Windows on the same PC.** Do not rely on `localhost`. Microsoft's documentation shows
  localhost access for TCP-style apps only and makes no promise for UDP, and community reports say
  UDP is not forwarded in NAT mode. Use mirrored mode (below), or bind mogbus to `0.0.0.0` and send
  the datagrams to the WSL IP address.
- **Using the WSL IP address.** Connections to that address count as coming from the LAN, so the app
  must accept LAN connections: set `(bind "0.0.0.0")`. Get the address from Windows with
  `wsl hostname -I` (capital `I`).
- **The WSL address can change** when the WSL VM restarts. Look it up again and update the
  `portproxy` rule (delete it and add it again).

### Other machines on the network

WSL2 is not reachable from the LAN by default. For TCP, forward a Windows port to the WSL address
with `netsh` (PowerShell as administrator) and allow the port in the Windows firewall:

```powershell
netsh interface portproxy add v4tov4 listenport=1502 listenaddress=0.0.0.0 connectport=1502 connectaddress=(wsl hostname -I)
netsh interface portproxy delete v4tov4 listenport=1502 listenaddress=0.0.0.0
```

The second command removes the rule again. `portproxy` only handles TCP (its syntax takes
`protocol=tcp`), so it does not help for `udp`, `ascii-udp` or `rtu-udp`: use mirrored mode for
those. mogbus needs `(bind "0.0.0.0")` here.

## WSL2 in mirrored mode

Mirrored mode needs Windows 11 22H2 or later. It copies the Windows network interfaces into Linux.
Create or edit `%UserProfile%\.wslconfig` on Windows:

```ini
[wsl2]
networkingMode=mirrored
```

Then run `wsl --shutdown` and start WSL again. In this mode Microsoft documents that Windows and WSL
can reach each other through `localhost` (`127.0.0.1`; `::1` is not supported), and that you can
connect to WSL from your LAN. `localhostForwarding` is ignored.

- **Same PC:** the default `(bind "127.0.0.1")` is enough; connect to `127.0.0.1:1502` from Windows.
  Microsoft's pages do not describe UDP separately for mirrored mode; test it with your client.
- **Other machines:** use `(bind "0.0.0.0")` and connect to the Windows PC address. The Hyper-V
  firewall blocks inbound connections to WSL by default, so allow them in PowerShell as
  administrator, for example for UDP port 1502 (use `-Protocol TCP` for TCP):

  ```powershell
  New-NetFirewallHyperVRule -Name "mogbus-udp" -DisplayName "mogbus UDP 1502" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 1502
  ```

  To allow all inbound connections instead, use
  `Set-NetFirewallHyperVVMSetting -Name '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -DefaultInboundAction Allow`.
  The normal Windows firewall rules apply as well.

## See also

- [Docker Desktop](windows-docker.md)
- [Troubleshooting](windows-troubleshooting.md)
- [Transports](../transports/transports.md#bind-addresses)
