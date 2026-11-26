# `(fault (kind disconnect))`

Drops the response and closes the client connection.

## Syntax

```scheme
(fault (kind disconnect) [(percent N)])
```

## Default

`percent 100`

## Valid in

- `(device ...)` in `tcp`, `rtu-tcp` and `ascii-tcp`

## Behaviour

A disconnect implies a dropped response and additionally closes the connection after the request has
been processed. Silent responses (listen-only mode and requests that produce no response) never close
the connection. It is rejected at load time in `rtu-pty`, `rtu-device`, `ascii-pty`, `ascii-device`,
`udp`, `ascii-udp` and `rtu-udp`: serial server fds are never closed by a fault, and UDP has no
connection. The same rule applies to a [state overlay](../machines/state-fault.md).

## Errors

- `(fault (kind disconnect)) is not valid in ... mode`: use `tcp`, `rtu-tcp` or `ascii-tcp` (not `udp`, `ascii-udp` or `rtu-udp`), or remove the fault; this also applies to state overlays.

## Examples

```scheme
(server (mode tcp))
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind disconnect) (percent 10)))   ; 1 in 10 answers closes the connection
```

## See also

- [drop](drop.md)
