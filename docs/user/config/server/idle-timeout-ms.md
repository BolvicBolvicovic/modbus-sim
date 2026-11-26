# `(server (idle-timeout-ms ...))`

Closes a client connection that makes no I/O progress for a while.

## Syntax

```scheme
(idle-timeout-ms N)
```

## Default

`0` (disabled)

## Valid in

- `tcp`
- `rtu-tcp`
- `ascii-tcp`

## Behaviour

`N` is 0-86400000 milliseconds; `0` disables the timeout. A connection with a delayed response
pending is not considered idle. In a TLS listener the [handshake deadline](tls-handshake-timeout-ms.md)
is independent of this option, and the timeout still defaults to `0`.

## Examples

```scheme
(server (mode tcp) (idle-timeout-ms 30000))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [handshake-timeout-ms](tls-handshake-timeout-ms.md)
