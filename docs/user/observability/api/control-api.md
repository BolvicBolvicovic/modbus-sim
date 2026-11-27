# Runtime control API

With `(control on)` the HTTP endpoint also serves a JSON API under `/api/` that changes the running server: list the configured points, read and write registers and bits, set a point's decoded value, and add or remove faults.

It is off by default, and while it is off every `/api/` path answers 404.

```scheme
(server (mode tcp) (port 1502)
  (http (port 9102) (bind "127.0.0.1") (control on)))

(device (name "Meter") (unit-id 1) (holding 0 (static 42)) (holding 10 (type float32) (static 21.5)))
```

> **Warning:** the API has no authentication. Anyone who can reach the port can change device state
> and inject faults, and the server logs a `WARN` at start to say so. Keep `bind` on loopback (the
> default), or put it behind a reverse proxy or firewall that does the access control before
> exposing it.

## Security

Besides the missing authentication, `/api/` checks each request against browser attacks: the
`Host` header must name this server (loopback, the bind address or an
[allowed-host](../../config/server/http-allowed-host.md)) or the answer is 403, and a state-changing
request with an `Origin` header must come from the same `http://host:port` with
`Content-Type: application/json`. Requests without `Origin` (curl, scripts) pass. Details are in
[Web UI](../web-ui.md#security).

## Conventions

- Requests and responses are JSON. A request body is at most 1024 bytes and must come with a
  `Content-Length`. It is a flat object with only the fields shown on the endpoint pages; any other
  key is an error.
- Everything runs on the server thread between Modbus requests, so a change is in effect for the
  very next request.
- `{space}` is `coil`, `discrete`, `input` or `holding`; `{unit}` and `{address}` are decimal.
- Errors have the form `{"error":"..."}`.
- The endpoint's limits still apply: 4 connections, 2 s per connection, one request each.
- `mogbus_control_requests_total{result="ok"|"error"}` counts these requests (only with control on).

## Status codes

| Status | Meaning |
|---|---|
| 200 | Done |
| 400 | Malformed JSON, an unknown or misplaced field, a missing required field, a bad number in the URL, or `count`, `space`, `from` or `limit` out of range |
| 403 | `Host` not allowed, or a cross-origin or non-JSON request with an `Origin` header (see Security) |
| 404 | Unknown path, unit, space or fault kind, or `DELETE` of a fault that is not set |
| 405 | Wrong method for that path |
| 413 | `Content-Length` over 1024 |
| 422 | The device refuses: undefined or out-of-range address, a point driven by a machine or by the host, or a fault value out of range or invalid for this transport |

## Two ways to write

- **Raw write** (`PUT /api/devices/{unit}/{space}/{address}`) works like a client write that also
  works on every space, including the read-only ones and points configured `(access read-only)`: you
  play the physical side. A ramp or model that is written becomes static, and machine `on-write`
  handlers run.
- **Value write** (`.../value`) sets the decoded number of a point by its first address, like a
  machine output. The point stays at that value until the device restarts, and Modbus writes to it
  get exception 02.

```bash
curl -s -X PUT -d '{"values":[1234]}' http://127.0.0.1:9102/api/devices/1/holding/0
curl -s http://127.0.0.1:9102/api/devices/1/holding/0?count=1
curl -s -X PUT -d '{"exception_code":6}' http://127.0.0.1:9102/api/devices/1/faults/exception
curl -s -X DELETE http://127.0.0.1:9102/api/devices/1/faults/exception
```

## See also

- [Health, readiness and metrics endpoint](../http-endpoint.md)
- [Using the control API from Python](../../python/python-observe.md#step-7-logs-metrics-and-the-control-api)
- [Config reference: http](../../config/server/index.md)
