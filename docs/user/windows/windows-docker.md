# Docker Desktop

Build the image from a PowerShell prompt and publish the port. What you publish with `-p` decides who can connect.

Build and run the image as described in [Run mogbus in Docker](../deployment/docker.md), from a
checkout of the repository:

```powershell
docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
docker run --rm -p 127.0.0.1:1502:1502 mogbus:0.0.1
```

The container config binds `0.0.0.0` inside the container, so the published port works.

## Who can connect

| Who connects | Option |
|---|---|
| Windows programs on the same PC | `-p 127.0.0.1:1502:1502`, then connect to `localhost:1502` |
| Other machines on the network | `-p 1502:1502` (no host address means all host interfaces); allow the connection in the Windows firewall |
| UDP (`mode udp`, `ascii-udp`, `rtu-udp`) | add the protocol: `-p 127.0.0.1:1502:1502/udp`; to publish both, repeat `-p` for `/tcp` and `/udp` |

For UDP use a config with `(server (mode udp) (port 1502) (bind "0.0.0.0"))`, mounted over
`/etc/mogbus/mogbus.scm` as shown in [Run mogbus in Docker](../deployment/docker.md#use-your-own-config).
The bind address inside the container must be `0.0.0.0`, like in the default container config;
`127.0.0.1` inside the container cannot be reached through a published port.

## Firewall and UDP

Inbound traffic goes through the Docker Desktop backend process (`com.docker.backend.exe`), so a
Windows firewall rule or prompt applies to that program. Docker's documentation describes the `/udp`
syntax but says nothing specific about UDP on Docker Desktop for Windows, so test UDP with your own
client.

A container reaches a program running on Windows through the name `host.docker.internal`. mogbus is a
server, so you rarely need this.

## See also

- [Reaching mogbus from Windows](windows-networking.md)
- [Running mogbus on Windows](windows-route.md)
