# `(server (http (allowed-host ...)))`

Adds a host name that the `Host` check of `/api/` and the web UI accepts.

## Syntax

```scheme
(allowed-host "NAME")
```

## Default

none

## Valid in

- all modes, inside `(http ...)`

## Behaviour

`/api/` and the web page answer 403 unless the request's `Host` header (port ignored, case
ignored) is `localhost`, `127.0.0.1`, `[::1]`, the specific `bind` address of the endpoint, or a
name given here. When the endpoint is bound to a wildcard (`0.0.0.0` or `::`), any IP literal is
accepted too, so a DNS name (a container name, a reverse proxy host) must be listed explicitly.
This is the defence against DNS rebinding. `/healthz`, `/readyz` and `/metrics` are never checked.

The key may be repeated, up to 8 names. A name is made of letters, digits, `.`, `-` and `_`; no
port and no scheme. All names together take fewer than 256 characters. A request without a `Host`
header (HTTP/1.0) passes.

## Errors

- `http: at most 8 allowed-host names`: remove names.
- `http: allowed-host 'NAME' must be a host name`: use letters, digits, `.`, `-` and `_` only, with no port, and keep all names under 256 characters in all.

## Examples

```scheme
(server (mode tcp) (port 1502)
  (http (bind "0.0.0.0") (ui on) (allowed-host "sim.lab") (allowed-host "mogbus")))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [ui](http-ui.md)
- [Web UI](../../observability/web-ui.md)
- [Runtime control API](../../observability/api/control-api.md)
