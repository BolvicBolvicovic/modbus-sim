# `(server ...)`

Selects the transport and the listen, serial, timing, TLS and HTTP options of one server; repeat it, with an `(id ...)` each, to run several servers from one file.

## Syntax

```scheme
(server (mode NAME) [(port N)] [(bind "ADDR")] [(seed N)] ...)
```

## Default

If the form is omitted: `(server (mode tcp) (port 1502) (bind "127.0.0.1"))`.

## Valid in

- top level, once for a single server, up to 8 times with an [id](id.md) each

## Behaviour

Only the keys listed for the selected mode are accepted. `mode` is allowed in every mode; a key that
is present but not allowed is rejected at load time, even if its value would be the default (for
example `(stop-bits 1)` in `tcp` mode). With several servers the devices choose theirs with
[servers](../device/servers.md); without a `(server ...)` form there is one default server.

## Errors

- `too many (server ...) forms (max 8)`: a file holds at most 8 servers.
- `server N has no (id "NAME"); every server needs one when the file has several`: name every server with [id](id.md).
- `server: parity is not valid in tcp mode`: the entry is not allowed by the selected mode; move it to a mode that supports it or remove it.

## Examples

```scheme
(server (mode tcp) (port 1502) (bind "127.0.0.1") (seed 1))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [mode](mode.md)
- [port](port.md)
- [bind](bind.md)
- [id](id.md)
