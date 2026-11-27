# Web UI

A browser UI is served by the HTTP endpoint. This is the first version: a small page that lists the
devices of the running server (it reads `/api/devices`). It is off by default.

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
