# Run mogbus in Docker

mogbus ships a `Dockerfile`. The image runs as an unprivileged user, needs no writable filesystem and no capabilities, and has a built-in health check.

On Windows, see [Docker Desktop](../windows/windows-docker.md) for the networking details.

## Build and run

```bash
docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
docker run --rm -p 127.0.0.1:1502:1502 mogbus:0.0.1
./tools/modbus_probe.py holding 0 2        # from the host
```

Podman works the same way (`podman build`, `podman run`, and `podman kube play` for the Kubernetes
example). Build with `podman build --format docker ...`: the default OCI image format has no
`HEALTHCHECK`, so podman would drop it. The image is about 85 MB.

## Use your own config

The container runs `/etc/mogbus/mogbus.scm` by default. Run a profile or your own config instead:

```bash
docker run --rm -p 127.0.0.1:1502:1502 mogbus:0.0.1 /usr/share/mogbus/profiles/power-meter.scm
docker run --rm -p 127.0.0.1:1502:1502 -v "$PWD/my.scm:/etc/mogbus/mogbus.scm:ro" mogbus:0.0.1
```

The profiles bind `127.0.0.1` (see below), so use a copy with `(bind "0.0.0.0")`, or the default
config, for anything that must be reachable. A mounted config must be readable by UID 10001. TLS
certificates and keys are mounted the same way and referenced by absolute path in `(tls ...)`;
`(tls ...)` servers default to port 802, so publish that port or set `(port 1502)`.

## The server must bind 0.0.0.0

mogbus binds `127.0.0.1` by default, which inside a container is unreachable through a published
port. The container config therefore sets `(bind "0.0.0.0")`; the same is needed in any config you
mount. This opens the port to everything that can reach the container, so publish it to loopback
(`-p 127.0.0.1:1502:1502`) unless remote clients are intended.

`EXPOSE` lists 1502/tcp and 1502/udp. Use the transports `tcp`, `rtu-tcp` and `ascii-tcp` on TCP and
`udp`, `rtu-udp` and `ascii-udp` on UDP (publish with `-p 1502:1502/udp`).

## Serial devices

Serial and PTY modes (`rtu-device`, `ascii-device`) need the host device passed in with
`--device /dev/ttyUSB0` and a group or ownership that lets UID 10001 open it (`--group-add`). The PTY
modes create a pseudo-terminal that exists only inside the container. Neither can be checked by
`--health`.

## What is in the image

| Path | Content |
|---|---|
| `/usr/local/bin/mogbus` | the binary (`ENTRYPOINT`) |
| `/etc/mogbus/mogbus.scm` | default config, `deploy/docker/mogbus.scm` (`CMD` argument) |
| `/usr/share/mogbus/profiles/` | the ready-made profiles |

The image is multi-stage. The build stage (`debian:trixie`, pinned by tag and by digest) installs
clang, make, pkg-config and libssl-dev and runs the normal prod `make` (hardened `-O2`, PIE,
`WITH_TLS=1`). The runtime stage is `gcr.io/distroless/base-debian13` (pinned by digest), about
10 MB compressed and 29 MB on disk against 85 MB for `debian:trixie-slim`. It is built from the
same Debian 13 packages (glibc, `libssl3t64`), so the binary runs as linked, and it contains no
shell, package manager or other tools: `docker exec` has nothing to run except `mogbus` itself
(for example `docker exec CONTAINER /usr/local/bin/mogbus --health /etc/mogbus/mogbus.scm`). The
image sets the numeric user `10001:10001` itself rather than distroless's `nonroot` (65532).

The system CA bundle shipped in the base is not used: mogbus reads only the CA files named in its
own config. To move to a newer base, change the digest in the runtime `FROM` line (and the build
stage's tag and digest together if the Debian release changes) and the `base.name` label.

## See also

- [Health check](health-check.md)
- [Container hardening and limits](container-hardening.md)
- [Docker Compose](docker-compose.md)
- [Kubernetes](kubernetes.md)
