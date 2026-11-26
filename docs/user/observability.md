# Observability

mogbus reports what it does in two ways: an HTTP endpoint with health, readiness and Prometheus
metrics, and log lines that can be plain text or JSON. Both are off or quiet by default.

## HTTP endpoint

Add `(http ...)` to the `(server ...)` section, in any mode:

```scheme
(server (mode tcp) (port 1502)
  (http (port 9102) (bind "127.0.0.1")))

(device (name "Meter") (unit-id 1) (holding 0 (static 42)))
```

`port` defaults to 9102 and `bind` to `"127.0.0.1"`; use `"0.0.0.0"` (or `"::"`) to reach it from
another machine or from outside a container. In the TCP modes the port must differ from the Modbus port.
The endpoint is served by the same loop that answers Modbus requests.

| Path | Answer |
|---|---|
| `/healthz` | `200 ok` whenever the server loop runs. If the loop stalls, the request times out, so it suits a liveness probe. |
| `/readyz` | `200 ready` while the server accepts requests; `503 not ready` before it starts and while it stops. Use it as a readiness probe. |
| `/metrics` | The metrics below, in the Prometheus text format. |

Other paths get 404 and methods other than GET and HEAD get 405 (the optional [runtime control](#runtime-control) API under `/api/` is the exception). A connection is closed after one
response or after 2 s, and at most 4 clients are served at once; this is meant for probes and
scrapers, not as a general web server.

```bash
curl -s http://127.0.0.1:9102/metrics | grep '^mogbus_requests_total'
```

A Prometheus scrape job for it:

```yaml
scrape_configs:
  - job_name: mogbus
    static_configs:
      - targets: ["127.0.0.1:9102"]
```

## Runtime control

With `(control on)` the endpoint also serves a JSON API under `/api/` that changes the running
server: read and write registers and bits, set a point's decoded value, and add or remove faults.
It is off by default, and while it is off every `/api/` path answers 404.

```scheme
(server (mode tcp) (port 1502)
  (http (port 9102) (bind "127.0.0.1") (control on)))

(device (name "Meter") (unit-id 1) (holding 0 (static 42)) (holding 10 (type float32) (static 21.5)))
```

**Security:** the API has no authentication. Anyone who can reach the port can change device state
and inject faults, and the server logs a WARN at start to say so. Keep `bind` on loopback (the
default), or put it behind a reverse proxy or firewall that does the access control before exposing it.

Requests and responses are JSON. A request body is at most 1024 bytes and must come with a
`Content-Length`; it is a flat object with only the fields shown below, and any other key is
an error. Everything runs on the server thread between Modbus requests, so a change is in
effect for the very next request.

| Request | Body | Answer |
|---|---|---|
| `GET /api/devices` | | `[{"unit":1,"name":"Meter","points":{"coil":0,"discrete":0,"input":0,"holding":2}}]` |
| `GET /api/devices/{unit}/{space}/{address}?count=N` | | `{"values":[42]}`: raw 16-bit values, 0 or 1 for bits; `count` defaults to 1, at most 125 registers or 2000 bits |
| `PUT /api/devices/{unit}/{space}/{address}` | `{"values":[1,2]}` | `{"ok":true}`; written as a client would, see below |
| `GET /api/devices/{unit}/{space}/{address}/value` | | `{"value":21.5}`: the decoded value of the point that starts at `address` |
| `PUT /api/devices/{unit}/{space}/{address}/value` | `{"value":22.25}` | `{"ok":true}`; the point becomes host-owned, see below |
| `GET /api/devices/{unit}/faults` | | `{"faults":[{"kind":"delay","percent":100,"scheduled":false,"delay_ms":250}]}` |
| `PUT /api/devices/{unit}/faults/{kind}` | `{"percent":100,...}` | `{"ok":true}`; replaces that kind |
| `DELETE /api/devices/{unit}/faults/{kind}` | | `{"ok":true}` |

`{space}` is `coil`, `discrete`, `input` or `holding`; `{unit}` and `{address}` are decimal.

- **Raw write** works like a client write that also works on every space, including the read-only ones and
  points configured `(access read-only)` (you play the physical side): a ramp or model that is written becomes static, and machine `on-write`
  handlers run. Bits take 0 or 1, registers 0-65535; the write is all-or-nothing and refused
  (422) if an address is undefined or a point is driven by a machine or was claimed with `/value`.
- **Value write** (`/value`) sets the decoded number of a point by its first address, like a machine
  output: it works on every space, the point stays at that value until the device restarts, and
  Modbus writes to it get exception 02. For bit points a nonzero value means on.
- **Faults** take the same fields and rules as `(fault ...)` in the config: `percent` (0-100, default
  100) for every kind, plus `delay_ms` (1-60000, required) for `delay`, `exception_code` (1-8, 10, 11,
  required) for `exception`, and `mode` (`pdu`, `register` or `checksum`, default `pdu`) for `corrupt`.
  `checksum` is refused in `tcp` and `udp` and `disconnect` outside `tcp`, `rtu-tcp` and `ascii-tcp`,
  as in the config. Schedules cannot be set over the API: a PUT always fires by `percent`, and replaces
  a scheduled fault of the same kind from the config. `GET` lists only the faults of the device
  itself, not those a machine state adds. Faults set here last until the process exits; a device `restart`
  fault resets the points and schedules but keeps them.

| Status | Meaning |
|---|---|
| 200 | Done |
| 400 | Malformed JSON, an unknown or misplaced field, a missing required field, a bad number in the URL, or `count` out of range |
| 404 | Unknown path, unit, space or fault kind, or `DELETE` of a fault that is not set |
| 405 | Wrong method for that path |
| 413 | `Content-Length` over 1024 |
| 422 | The device refuses: undefined or out-of-range address, a point driven by a machine or by the host, or a fault value out of range or invalid for this transport |

Errors have the form `{"error":"..."}`. The endpoint's limits still apply: 4 connections, 2 s per
connection, one request each.

```bash
curl -s -X PUT -d '{"values":[1234]}' http://127.0.0.1:9102/api/devices/1/holding/0
curl -s http://127.0.0.1:9102/api/devices/1/holding/0?count=2
curl -s -X PUT -d '{"exception_code":6}' http://127.0.0.1:9102/api/devices/1/faults/exception
curl -s -X DELETE http://127.0.0.1:9102/api/devices/1/faults/exception
```

`mogbus_control_requests_total{result="ok"|"error"}` counts these requests (only with control on).

## Metrics

Counters start at zero when the server starts. Series with a `unit` label are per configured device.

| Metric | Type | Meaning |
|---|---|---|
| `mogbus_info{version,transport}` | gauge | Always 1; the version and the `(server (mode ...))` of this server |
| `mogbus_ready` | gauge | 1 while the server accepts requests, as `/readyz` |
| `mogbus_start_time_seconds` | gauge | Start time, Unix seconds |
| `mogbus_requests_total{fc}` | counter | Decoded requests by function code, including requests to unknown units and broadcasts |
| `mogbus_responses_total{result}` | counter | Requests by outcome: `ok`, `exception`, `none` (no response by protocol: broadcast, unknown RTU/ASCII unit), `dropped` (by a drop or disconnect fault) |
| `mogbus_frames_rejected_total` | counter | Frames rejected before reaching a device: bad CRC or LRC, malformed, oversized |
| `mogbus_response_latency_seconds` | histogram | Time from a request to its scheduled response, so it mainly shows injected `delay` faults; buckets from 1 ms to 10 s |
| `mogbus_connections_active` | gauge | Open client connections (TCP modes) |
| `mogbus_connections_total` | counter | Accepted client connections (TCP modes) |
| `mogbus_control_requests_total{result}` | counter | Runtime control API requests, `ok` or `error` (HTTP status 400 and up); present only with `(control on)` |
| `mogbus_device_info{unit,name}` | gauge | Always 1, one series per device |
| `mogbus_device_requests_total{unit}` | counter | Requests that reached the device |
| `mogbus_device_responses_total{unit,result}` | counter | Those requests by outcome, as above |
| `mogbus_device_exceptions_total{unit,code}` | counter | Exception responses by exception code, whether from the device or from an `exception` fault |
| `mogbus_device_faults_total{unit,kind}` | counter | Injected faults that fired, by kind (`delay`, `drop`, `exception`, `corrupt`, `disconnect`, `restart`, `sensor-disconnect`); configured kinds appear even at 0 |
| `mogbus_device_comm_errors_total{unit}` | counter | Bus communication errors the device counted (FC 08 sub-function 0x000C); 16 bits, so it wraps after 65535 |

Notes:

- `mogbus --health` is a client too: each probe counts as an FC 08 request (`fc="8"`) to the first device.
- The UDP and serial modes have no connections, so the connection metrics stay at 0 there.
- A program that embeds the library can get the same text with `mogbus_server_metrics()` and export it
  itself (see [library.md](library.md)).

## Logs

Logs go to stderr. `-v` / `--verbose` adds one DEBUG line per request and per TCP connect and
disconnect. `--log-format json` writes every line as a JSON object instead of text:

```bash
./mogbus -v --log-format json my-device.scm
```

```json
{"ts":"2026-11-26T09:42:52.164Z","level":"debug","msg":"tcp 127.0.0.1:38042 unit=2 fc=0x03 -> exception 0x02","event":"request","transport":"tcp","peer":"127.0.0.1:38042","unit":2,"fc":3,"result":"exception","exception":2,"latency_ms":0,"faults":[]}
```

Every line has `ts` (UTC, millisecond precision), `level` (`debug`, `info`, `warn`) and `msg` (the text line).
Request lines (`"event":"request"`) add:

| Field | Meaning |
|---|---|
| `transport` | the server mode, e.g. `tcp`, `rtu-udp` |
| `peer` | client `ip:port`; absent on serial and PTY byte streams |
| `unit` | unit id from the frame (0 = broadcast) |
| `fc` | function code |
| `result` | `ok`, `exception`, `none` or `dropped`, as in `mogbus_responses_total` |
| `exception` | exception code, 0 when the result is not `exception` |
| `latency_ms` | scheduled response delay from a `delay` fault |
| `faults` | fault kinds that fired for this request, e.g. `["delay","corrupt"]` |

Connection lines have `"event":"connect"` or `"event":"disconnect"` with `transport` and `peer`. A program
that embeds the library selects the format with `mogbus_set_log_format()`; a log sink then receives the
whole JSON object as its message.
