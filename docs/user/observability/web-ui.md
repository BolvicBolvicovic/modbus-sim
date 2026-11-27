# Web UI

A browser UI is served by the HTTP endpoint. It is one self-contained page (no external resources,
no frameworks) over the [control API](api/control-api.md) that shows and changes a running server:
tables and values, and faults. It is off by default.

## Opening it

Any of these turns it on; the UI needs the [runtime control API](api/control-api.md), so each also
turns control on, with the usual WARN in the log.

- Command line: `mogbus --web CONFIG.scm` (or `-w`). Without an `(http ...)` section in the config
  it listens on `127.0.0.1` port `9102`; with one it reuses that bind and port.
- Config: `(server (http (ui on)))`, see [ui](../config/server/http-ui.md).
- Library: `http_ui` in `mogbus_server_options_t` (Python `ServerOptions.http_ui`), with
  `http_bind_address` set.

The start-up summary (and `--check`) prints the address, for example
`web UI on http://127.0.0.1:9102/`. Open it in a browser on the same machine.

## What it shows and edits

- **Device**: the selector lists the devices (`/api/devices`).
- **Points**: one tab per table (`coil`, `discrete`, `input`, `holding`) lists the configured points
  from [List points](api/api-list-points.md): address, type (strings show their register count), kind
  (`static`, `ramp`, `model`, `clock`, `machine`), a read-only marker, the decoded value and the raw
  registers. Values are decoded in the browser from raw reads, honouring each point's word and byte
  order. The list is paged by 50 points.
- **Table / Plot**: a switch above the list. `Table` is the list; `Plot` shows a line chart of the
  points ticked in the `Plot` column of the list (strings cannot be plotted). Up to 8 points, of any
  table, can be plotted; the selection belongs to the device and is cleared when you switch device.
  Each series is sampled on every refresh, wherever it is (another page or table), in both views, and
  keeps the samples of the last 5 minutes (at most 3000); the time axis grows with the samples up to 5 minutes, and the vertical
  axis scales to the plotted values, bits as steps, and a gap where a read failed. The legend gives colour,
  table and address, type, latest value, and a `×` button to remove the series. Samples are
  kept in the browser only and are lost on reload.
- **Refresh**: the visible page and the plotted points are read again every second by default (the
  `Refresh every` selector offers 100 ms, 250 ms, 500 ms, 1 s, 2 s, 5 s and 10 s, applies at once to
  both views and is remembered in the browser; a read still running makes the next tick skip), one raw read per run of adjacent points
  (at most 125 registers or 2000 bits), only while the browser tab is visible. `Pause` stops it and
  `Refresh now` reads once. Switching device or table, or paging, reloads the list.
- **Write**: pick an address (click it in the list) and either set the decoded value of the point
  ([Set a value](api/api-set-value.md)) or write raw registers from that address, comma-separated
  ([Write values](api/api-write-values.md)). A refusal from the server, for example a string point,
  a machine-driven point or an undefined address, is shown under the form.
- **Faults**: lists the faults of the device with a `Clear` button each, and a form to set one:
  kind, `percent`, and `delay_ms`, `exception_code` or `mode` where the kind takes them
  ([Set a fault](api/api-set-fault.md)). Faults from a schedule are marked `scheduled`; setting a
  fault over one replaces it.

Writes change the running device exactly as the API does: a raw write turns a ramp or model into a
static value, and a value write holds the point until the device restarts.

## Security

There is no authentication: anyone who can reach the port can change device state and inject faults.
The defaults keep it on loopback. Do not expose it on an untrusted network; put it behind a reverse
proxy or firewall that does the access control.

Because a browser on the same machine can be tricked into talking to a local server, `/api/` and the
page also check each request:

- **Host** (DNS rebinding): the `Host` header, port ignored, must be `localhost`, `127.0.0.1`,
  `[::1]`, the specific bind address, or a name from [allowed-host](../config/server/http-allowed-host.md).
  With a wildcard bind (`0.0.0.0`, `::`) any IP literal is also accepted, so a DNS name must be
  listed with `allowed-host` (library: `http_allowed_hosts`, comma-separated). Otherwise the answer is 403.
  A request without a `Host` header (HTTP/1.0, for example `curl -0`) is allowed.
- **Origin** (CSRF): a request that changes state (anything but GET and HEAD) and carries an `Origin`
  header must have `Origin: http://<the request's Host>` and `Content-Type: application/json`;
  otherwise 403. Scripts and `curl`, which send no `Origin`, are not affected.

`/healthz`, `/readyz` and `/metrics` are never checked, so probes and Prometheus can use any name.

## See also

- [Health, readiness and metrics endpoint](http-endpoint.md)
- [Runtime control API](api/control-api.md)
- [Command line](../command-line.md)
