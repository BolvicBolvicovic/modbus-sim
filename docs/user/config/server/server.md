# `(server ...)`

Selects the transport and the listen, serial, timing, TLS and HTTP options for the whole file.

## Syntax

```scheme
(server (mode NAME) [(port N)] [(bind "ADDR")] [(seed N)] ...)
```

## Default

If the form is omitted: `(server (mode tcp) (port 1502) (bind "127.0.0.1"))`.

## Valid in

- top level, at most once

## Behaviour

Only the keys listed for the selected mode are accepted. `mode` is allowed in every mode; a key that
is present but not allowed is rejected at load time, even if its value would be the default (for
example `(stop-bits 1)` in `tcp` mode).

## Errors

- `the file has more than one (server ...); only one is allowed`: keep a single `(server ...)` block.
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
