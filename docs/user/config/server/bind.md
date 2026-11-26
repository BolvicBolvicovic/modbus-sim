# `(server (bind ...))`

Sets the address the server listens on.

## Syntax

```scheme
(bind "ADDRESS")
```

## Default

`"127.0.0.1"`

## Valid in

- `tcp`
- `rtu-tcp`
- `ascii-tcp`
- `udp`
- `ascii-udp`
- `rtu-udp`

## Behaviour

`ADDRESS` must be a plain IPv4 or IPv6 literal: no hostnames and no zone/scoped link-local suffix
such as `"fe80::1%eth0"` (the validator uses `inet_pton()`, which does not accept `%iface`).

- `"0.0.0.0"`: all IPv4 interfaces (explicit opt-in for remote clients).
- `"127.0.0.1"`: this machine over IPv4 only (the default).
- `"::"`: all IPv6 interfaces (explicit opt-in).
- `"::1"`: this machine over IPv6 only.

IPv6 listen sockets set `IPV6_V6ONLY`, so `"::"` serves IPv6 clients only. TCP listeners and UDP
sockets default to loopback; remote connections require an explicit `(bind "0.0.0.0")` or
`(bind "::")`. There is no way to listen on both families at once; there is no dual-stack option.

> **Warning:** binding to a wildcard address exposes the simulator to the network.

## Examples

```scheme
(server (mode tcp) (bind "0.0.0.0") (port 1502))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [port](port.md)
- [http](http.md)
