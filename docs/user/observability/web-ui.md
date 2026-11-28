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

The start-up summary (and `--check=verbose`) prints the address, for example
`web UI on http://127.0.0.1:9102/`. Open it in a browser on the same machine.

## Using it

The page has a device selector at the top, a `Table`/`Plot` switch, a refresh control and a faults
panel. Everything it does goes through the [control API](api/control-api.md).

### Pick a device

The selector lists the configured devices. Switching device reloads the list and clears the plot
selection, which belongs to the device.

### Browse points

One tab per table (`coil`, `discrete`, `input`, `holding`) lists the configured points, 50 per
page, from [List points](api/api-list-points.md). Each row shows:

| Column | Content |
|---|---|
| Address | as sent on the wire |
| Type | the data type; strings show their register count |
| Kind | `static`, `ramp`, `model`, `clock` or `machine`, plus a read-only marker |
| Value | decoded in the browser from the raw registers, honouring word and byte order |
| Raw | the registers as read |
| Note | the [`(note ...)`](../config/points/point.md) of the point in muted text; the column is hidden when no point of the table has one |
| Plot | a checkbox that puts the point on the chart (not for strings) |

### Keep values fresh

The visible page is read again every second. The `Refresh every` selector goes from 10 ms to 10 s,
shows the rate actually achieved in the header and is remembered by the browser; a tick is skipped while the
previous read is still running. Reads are batched (one request per run of adjacent points) and only
happen while the browser tab is visible. `Pause` stops refreshing and `Refresh now` reads once.

### Change a value

Click an address in the list, then either set the decoded value of the point
([Set a value](api/api-set-value.md)) or write raw registers from that address, comma-separated
([Write values](api/api-write-values.md)). A refusal from the server (a string point, a
machine-driven point, an undefined address) is shown under the form.

Writes act on the running device as the API does: a raw write turns a ramp or model into a static
value, and a value write holds the point until the device restarts.

### Inject faults

The faults panel lists the faults of the device with a `Clear` button each, and a form to set one:
the kind, `percent`, and `delay_ms`, `exception_code` or `mode` where the kind takes them
([Set a fault](api/api-set-fault.md)). Faults that come from a schedule are marked `scheduled`;
setting a fault over one replaces it.

### Plot values over time

Switch to `Plot` to chart up to 8 points of any table. Tick them in the `Plot` column, or use
`Add point`: pick a table, type in the box to filter its points by address or label, tick the ones
you want in the dropdown (click, or Up/Down and Enter) and press `Add ticked`. The legend shows
colour, address, type and latest value, with a `×` to remove a series.

Every plotted point is sampled on each refresh, in both views, and the browser keeps the last
5 minutes (at most 30 000 samples). They are lost on reload.

| To do this | Do this |
|---|---|
| Change the time window (1 s to 5 min) | `-`/`+` buttons or the vertical wheel over the chart; remembered by the browser |
| Follow now | `Live` button (pressed while following), End, or scroll to the newest sample |
| Look back in time | horizontal wheel or trackpad, Shift and the wheel, Left/Right keys (Home: oldest), or the scrollbar under the chart; this leaves live mode and the view stays at fixed times |
| Freeze the view | `Pause` |
| Measure | drag across the chart and release: a band stays until you click, press Esc or drag again, and the line under the chart gives `Δt` and each point's value at both edges and the difference |

Times on the axis, the window range and the measure line are elapsed since the plot started (`m:ss`, with milliseconds on short windows), not the clock of the machine. `Reset` clears the samples of every plotted point and restarts the time at `0:00`; the ticked points stay. The vertical axis fits the plotted values, bits are
drawn as steps and a failed read leaves a gap. Dense series are reduced to the minimum and maximum of
each pixel column, so drawing cost does not depend on the sampling rate.

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
