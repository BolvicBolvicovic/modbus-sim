# Health check

`mogbus --health CONFIG.scm [--timeout-ms N]` asks a running server whether it answers. It is what the Docker image uses as its `HEALTHCHECK`.

It loads the config, then sends one real Modbus request to the server the `(server ...)` section
describes and exits 0 if it is answered, 1 otherwise. The reason goes to stderr, for example
`unhealthy: connect 127.0.0.1:1502: Connection refused`. The default timeout is 2000 ms.

## What it sends

- The request is FC 08 sub-function 0x0000 (Return Query Data) to the unit id of the first
  configured device. It works without any configured points and does not change device state.
- It uses the configured transport: MBAP for `tcp` and `udp`, RTU frames with CRC for `rtu-tcp` and
  `rtu-udp`, ASCII frames with LRC for `ascii-tcp` and `ascii-udp`.
- A bind of `0.0.0.0` is probed through `127.0.0.1` and `::` through `::1`, so run it inside the
  container.
- A well-formed reply from that unit is healthy: the echo, or a Modbus exception (the server
  answered). A refused connection, a timeout, a closed connection or a malformed reply is unhealthy.

## What it cannot check

- **TLS is not exercised.** For a `tcp` server with `(tls ...)` the probe only opens a TCP
  connection. It has no client certificate, so it sends no request and does not do the TLS
  handshake: the check passes as soon as the port accepts the connection. A broken certificate or
  a misconfigured cipher list is not detected (`mogbus --check` finds those before you start).
- **Only the first device is asked.** Other devices, their points and their faults are not checked.
- Serial and PTY modes exit 1 with `health check needs a network mode`.
- The probe is itself a client: a TCP probe takes one of the 32 connection slots for a moment.

## Faults can fail the probe

Faults on the first device can make the probe fail: `drop`, a `delay` longer than the timeout and
`disconnect` produce no reply, and `corrupt` may produce an invalid one. Put a fault-free device first
when a profile uses faults and the container is supervised. `restart` and `sensor-disconnect` do not
affect the probe.

## In the image

The image runs it every 10 s (timeout 5 s, 5 s start period, 3 retries) with:

```text
HEALTHCHECK CMD ["/usr/local/bin/mogbus","--health","/etc/mogbus/mogbus.scm"]
```

If you pass a different config path to the container, override the health check to use the same
path (`--health-cmd`, or `healthcheck:` in Compose). `docker ps` shows `healthy`. Docker itself does
not restart an unhealthy container, so use an orchestrator, or `restart:` plus an autoheal tool, if
you want that.

The container's default config also enables the HTTP endpoint on port 9102 (`/healthz`, `/readyz`,
Prometheus `/metrics`); publish it with `-p 127.0.0.1:9102:9102`. The image keeps `--health` as its
`HEALTHCHECK` because it has no HTTP client. See
[Health, readiness and metrics endpoint](../observability/http-endpoint.md).

## See also

- [Run mogbus in Docker](docker.md)
- [Command line](../command-line.md)
- [Return Query Data (0x0000)](../modbus/diagnostics/sub-0000-return-query-data.md)
