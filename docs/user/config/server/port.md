# `(server (port ...))`

Sets the TCP or UDP port the server listens on.

## Syntax

```scheme
(port N)
```

## Default

`1502`, or `802` when `(tls ...)` is present

## Valid in

- `tcp`
- `rtu-tcp`
- `ascii-tcp`
- `udp`
- `ascii-udp`
- `rtu-udp`

## Behaviour

`N` is 1-65535. When [`(http ...)`](http.md) is on in a TCP mode, its port must differ from this one.

## Examples

```scheme
(server (mode tcp) (port 5020))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [bind](bind.md)
- [tls](tls.md)
