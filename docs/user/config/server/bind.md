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

`ADDRESS` is an IPv4 or IPv6 literal or a host name (letters, digits, `.`, `-` and `_`). A zone/scoped
link-local suffix such as `"fe80::1%eth0"` is not accepted.

A host name is kept as written in the config and resolved once, when the server is created (and by
`--check`, so a name that does not resolve fails with `bind address 'NAME' is not an IP address and
does not resolve to one`). The resolved IPv4 addresses are tried first, then the IPv6 ones, and the
server listens on the first address that binds; the others are not used, so `"localhost"` is
`127.0.0.1` even when the resolver lists `::1` first. A name that resolves to several addresses
therefore binds one of them, not all. The check for two servers on one port compares the text of
the addresses, so `"localhost"` and `"127.0.0.1"` are not recognised as the same listener (the second
bind fails when the server starts). The HTTP endpoint accepts a name the same way
([http](http.md)).

- `"0.0.0.0"`: all IPv4 interfaces (explicit opt-in for remote clients).
- `"127.0.0.1"`: this machine over IPv4 only (the default).
- `"::"`: all IPv6 interfaces (explicit opt-in).
- `"::1"`: this machine over IPv6 only.
- `"localhost"`: whatever the name resolves to, IPv4 first (normally `127.0.0.1`).

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
