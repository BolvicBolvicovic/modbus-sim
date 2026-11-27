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

- **Header**: the mogbus logo (icon and wordmark, inlined in the page; the icon is also the favicon).
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
  keeps the samples of the last 5 minutes (at most 30 000). The window is chosen with the `-`/`+`
  buttons or the vertical mouse wheel over the chart (1 s, 2 s, 5 s, 10 s, 30 s by default, 1 min, 2 min
  or 5 min, remembered in the browser). By default the chart is `Live`: it follows now, with data
  entering on the right and leaving on the left (an empty left part means there is not that much history
  yet). Scrolling in time leaves live mode: use the horizontal wheel or trackpad, Shift and the wheel,
  the Left/Right arrow keys (Home goes to the oldest sample) when the chart has focus, or the history
  scrollbar under the chart. The view then stays at fixed absolute times while samples keep arriving,
  and the range shown next to the window size is updated; the `Live` button (pressed while following)
  or End, or scrolling to the newest sample, returns to now. Zooming keeps the time under the cursor
  fixed when not live. Time gridlines sit at absolute wall-clock times and are labelled with the time of
  day (with milliseconds below 1 s steps). Drag across the chart and release to measure: a shaded band
  between the two times stays until you click on the chart, press Esc or drag again, and the line under
  the chart gives the elapsed time (`Δt = 1.234 s`) and, for every plotted point, its value (nearest
  sample) at both edges and the difference. The band is fixed in time, so it moves with scrolling. The
  vertical axis scales to the plotted values, bits as steps, and a gap where
  a read failed. Dense series are reduced to the minimum and maximum of every pixel column, and drawing
  runs once per animation frame, independent of the sampling rate. `Pause` freezes the view. The legend
  gives colour, table and address, type, latest value, and a `×` button to remove the series. Below it,
  `Add point` picks a table; the box next to it filters that table's plottable points by address or
  label and opens a dropdown over the page (also with ArrowDown) listing them, for example `holding 10 ·
  float32 · ramp`. Tick several (click, or Up/Down and Enter) and press `Add ticked` to plot them all;
  with nothing ticked and a single point left after filtering, Enter or `Add ticked` adds that one. Esc,
  Tab or a click elsewhere closes the dropdown. At most 8 points
  are plotted, extra ones are skipped with a message. The points' checkboxes in the list follow. Samples are kept in the browser only and are lost on reload.
- **Refresh**: the visible page and the plotted points are read again every second by default (the
  `Refresh every` selector offers 10 ms, 20 ms, 50 ms, 100 ms, 250 ms, 500 ms, 1 s, 2 s, 5 s and 10 s, applies at once to
  both views and is remembered in the browser; a read still running makes the next tick skip, and the achieved rate is shown next to the selector), one raw read per run of adjacent points
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
