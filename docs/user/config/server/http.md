# `(server (http ...))`

Serves the HTTP endpoint with `/healthz`, `/readyz` and `/metrics`, and optionally a runtime control API.

## Syntax

```scheme
(http (port N) (bind "ADDR") (control on|off) (ui on|off) (allowed-host "NAME")...)
```

## Default

off (no endpoint). When present: port `9102`, bind `"127.0.0.1"`, control `off`, ui `off`.

## Valid in

- all modes

## Behaviour

- `/healthz`: 200 while the server loop runs.
- `/readyz`: 200 while serving, 503 before start and while stopping.
- `/metrics`: Prometheus text format: requests by function code, responses by outcome, rejected
  frames, response latency histogram, connections, and per device requests, exceptions, fired faults
  and comm errors.

In TCP modes the HTTP port must differ from the Modbus port. `(control on)` (default `off`) also
serves the unauthenticated `/api/` control API that reads and writes registers and sets or clears
faults at runtime, and logs a WARN at start; see the [runtime control API](../../observability/api/control-api.md).

`(ui on)` serves the [web UI](../../observability/web-ui.md) on `/` and turns control on too.
`/api/` and the UI check the `Host` header (and `Origin` on changes); a DNS name needs
[allowed-host](http-allowed-host.md).

> **Warning:** the control API has no authentication; keep it on loopback.

## Examples

```scheme
(server (mode tcp) (port 1502) (http (port 9102) (bind "127.0.0.1")))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [observability](../../observability/http-endpoint.md)
- [ui](http-ui.md)
- [allowed-host](http-allowed-host.md)
- [port](port.md)
