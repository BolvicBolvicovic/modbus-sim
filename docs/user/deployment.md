# Deployment

mogbus ships a `Dockerfile` and examples for Docker Compose and Kubernetes. The image runs as an
unprivileged user, needs no writable filesystem and no capabilities, and has a built-in health check.

## Build and run the image

```
docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
docker run --rm -p 127.0.0.1:1502:1502 mogbus:0.0.1
./tools/modbus_probe.py holding 0 2        # from the host
```

Podman works the same way (`podman build`, `podman run`, and `podman kube play` for the Kubernetes
example). Build with `podman build --format docker ...`: the default OCI image format has no
`HEALTHCHECK`, so podman would drop it. The image is about 85 MB.

The image is multi-stage. The build stage (`debian:trixie`, pinned by tag and by digest) installs clang,
make, pkg-config and libssl-dev and runs the normal prod `make` (hardened `-O2`, PIE, `WITH_TLS=1`). The
runtime stage is `debian:trixie-slim` (same release, so the `libssl3t64` it installs matches the library the
binary was linked against) with nothing added but that library. A distroless base would be smaller, but
keeping the builder's distribution gives an exact libssl ABI match and a shell for `docker exec`
debugging; no system CA bundle is installed because mogbus reads only the CA files named in its own config.
To move to a newer base, change the tag and digest in both `FROM` lines and the `base.name` label.

Layout inside the image:

| Path | Content |
|---|---|
| `/usr/local/bin/mogbus` | the binary (`ENTRYPOINT`) |
| `/etc/mogbus/mogbus.scm` | default config, `deploy/docker/mogbus.scm` (`CMD` argument) |
| `/usr/share/mogbus/profiles/` | the ready-made profiles |

Run a profile or your own config instead of the default:

```
docker run --rm -p 127.0.0.1:1502:1502 mogbus:0.0.1 /usr/share/mogbus/profiles/power-meter.scm
docker run --rm -p 127.0.0.1:1502:1502 -v "$PWD/my.scm:/etc/mogbus/mogbus.scm:ro" mogbus:0.0.1
```

The profiles bind `127.0.0.1` (see below), so use a copy with `(bind "0.0.0.0")` or the default config for
anything that must be reachable. A mounted config must be readable by UID 10001. TLS certificates and keys
are mounted the same way and referenced by absolute path in `(tls ...)`; `(tls ...)` servers default to
port 802, so publish that port or set `(port 1502)`.

### The server must bind 0.0.0.0

mogbus binds `127.0.0.1` by default, which inside a container is unreachable through a published port.
The container config therefore sets `(bind "0.0.0.0")`; the same is needed in any config you mount. This
opens the port to everything that can reach the container, so publish it to loopback
(`-p 127.0.0.1:1502:1502`) unless remote clients are intended. `EXPOSE` lists 1502/tcp and 1502/udp; use
the transports `tcp`, `rtu-tcp` and `ascii-tcp` on TCP and `udp`, `rtu-udp` and `ascii-udp` on UDP (publish
with `-p 1502:1502/udp`).

Serial and PTY modes (`rtu-device`, `ascii-device`) need the host device passed in with
`--device /dev/ttyUSB0` and a group or ownership that lets UID 10001 open it (`--group-add`); the PTY modes
create a pseudo-terminal that exists only inside the container. Neither can be checked by `--health`.

## Health check

`mogbus --health CONFIG.scm [--timeout-ms N]` loads the config, then sends one real Modbus request to the
server the `(server ...)` section describes and exits 0 if it is answered, 1 otherwise (the reason goes to
stderr, e.g. `unhealthy: connect 127.0.0.1:1502: Connection refused`). The default timeout is 2000 ms.

- The request is FC 08 sub-function 0x0000 (Return Query Data) to the unit id of the first configured
  device. It works without any configured points and does not change device state.
- It uses the configured transport: MBAP for `tcp` and `udp`, RTU frames with CRC for `rtu-tcp` and `rtu-udp`,
  ASCII frames with LRC for `ascii-tcp` and `ascii-udp`.
- A bind of `0.0.0.0` is probed through `127.0.0.1` and `::` through `::1`, so run it inside the container.
- A well-formed reply from that unit is healthy: the echo, or a Modbus exception (the server answered).
  A refused connection, a timeout, a closed connection or a malformed reply is unhealthy.
- A TLS server (`(tls ...)`) is only connected to: the probe has no client certificate, so a successful TCP
  connect counts as healthy and the handshake is not exercised.
- Faults on the first device can make the probe fail: `drop`, long `delay` (longer than the timeout) and
  `disconnect` produce no reply, and `corrupt` may produce an invalid one. Put a fault-free device first
  when a profile uses faults and the container is supervised. `restart` and `sensor-disconnect` do not
  affect the probe.
- The probe is itself a client: a TCP probe takes one of the 32 connection slots for a moment.
- Serial and PTY modes exit 1 with `health check needs a network mode`.

The image runs it every 10 s (timeout 5 s, 5 s start period, 3 retries) with
`HEALTHCHECK CMD ["/usr/local/bin/mogbus","--health","/etc/mogbus/mogbus.scm"]`. If you pass a different
config path to the container, override the health check to use the same path
(`--health-cmd`, or `healthcheck:` in Compose). `docker ps` shows `healthy`; Docker itself does not restart an
unhealthy container, so use an orchestrator or `restart:` plus an autoheal tool if you want that.

The container's default config also enables the HTTP endpoint on port 9102 (`/healthz`, `/readyz`,
Prometheus `/metrics`); publish it with `-p 127.0.0.1:9102:9102`. The image keeps `--health` as its
`HEALTHCHECK` because it has no HTTP client. See [observability.md](observability.md).

## Non-root, read-only, no capabilities

The image sets `USER 10001:10001` and needs nothing else:

- mogbus writes no files; logs go to stderr (`docker logs`), so `--read-only` works without tmpfs mounts.
- It binds a port above 1024 and needs no capability, so `--cap-drop ALL` and
  `--security-opt no-new-privileges` are safe.
- It handles SIGTERM and SIGINT itself and forks no children, so it works as PID 1: `docker stop` ends it
  within a poll tick with exit status 0, and there are no zombies to reap. `--init` is not needed.

```
docker run -d --name mogbus --read-only --cap-drop ALL --security-opt no-new-privileges \
  --memory 64m --cpus 0.5 --pids-limit 64 -p 127.0.0.1:1502:1502 mogbus:0.0.1
```

## Resource limits

mogbus is single-threaded (one `poll` loop), allocates only from mmap arenas created at start-up, and
allocates nothing per request. Measured on Linux with the `minimal` profile: about 10 MB resident (14 MB
peak at start-up while the config is parsed), one thread. Memory does not grow with traffic; at most 32 TCP clients
are served at once. The shipped examples therefore use generous limits:

| Limit | Value | Why |
|---|---|---|
| memory | 64 MiB (request 16 MiB in Kubernetes) | several times the working set, including many devices |
| CPU | 0.5 core | one thread; a simulator needs far less, a busy client cannot use more than one core |
| pids | 64 | one process, one thread; this only stops fork bombs from a compromised process |

Raise the memory limit for very large configs (many devices, machines or file records).

## Docker Compose

`deploy/docker-compose.yml` builds the image, publishes 1502 on loopback, bind-mounts
`deploy/docker/mogbus.scm` over the default config, and sets `user`, `read_only`, `cap_drop: [ALL]`,
`no-new-privileges`, the health check, `cpus`, `mem_limit`, `pids_limit` and `restart: unless-stopped`.

```
docker compose -f deploy/docker-compose.yml up --build
```

## Kubernetes

`deploy/kubernetes/mogbus.yaml` holds a ConfigMap with the config, a one-replica Deployment and a ClusterIP
Service. Build the image and push it to a registry the cluster can pull from, change `image:` to match, then
`kubectl apply -f deploy/kubernetes/mogbus.yaml`. The pod runs as UID 10001 with `runAsNonRoot`, a read-only
root filesystem, `allowPrivilegeEscalation: false`, all capabilities dropped and the `RuntimeDefault` seccomp
profile, with CPU/memory requests and limits. The config enables the HTTP endpoint on port 9102, so the
readiness probe is `httpGet /readyz` and the liveness probe `httpGet /healthz` (see
[observability.md](observability.md)), and the pod carries the common `prometheus.io/scrape`, `port` and
`path` annotations for Prometheus. Without `(http ...)`, use exec probes running `mogbus --health` on the
mounted config instead. The config is
mounted read-only at `/etc/mogbus`; change the ConfigMap and restart the pod (mogbus does not reload its
config).

Run one replica per simulated plant: every replica holds its own device state, so a Service in front of
several replicas would answer from different devices.
