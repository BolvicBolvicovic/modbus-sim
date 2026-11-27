# `(server (http (ui ...)))`

Serves the built-in web UI on `/` of the HTTP endpoint.

## Syntax

```scheme
(http (ui on|off))
```

## Default

`off`

## Valid in

- all modes, inside `(http ...)`

## Behaviour

`(ui on)` makes `GET /` (and `/index.html`) of the HTTP endpoint return the page. The page talks to
the [runtime control API](../../observability/api/control-api.md), so `(ui on)` turns
`(control on)` on as well, and the server logs the same WARN at start: neither has authentication.
With `(ui off)` the path `/` answers 404 as before.

`/api/` and the page check the request's `Host` header and, for requests that change state, its
`Origin`; see [Web UI](../../observability/web-ui.md) and [allowed-host](http-allowed-host.md).
The command-line option `--web` (`-w`) has the same effect without editing the config.

## Errors

- `(ui maybe): expected on or off (also yes/no, true/false, 1/0)`: use `on` or `off`.

> **Warning:** do not expose the UI on an untrusted network; it can change device state and inject faults.

## Examples

```scheme
(server (mode tcp) (port 1502) (http (port 9102) (ui on)))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [Web UI](../../observability/web-ui.md)
- [http](http.md)
- [allowed-host](http-allowed-host.md)
