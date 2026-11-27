# Modbus UDP (`udp`)

Modbus TCP's MBAP ADU over UDP: one complete ADU (7-byte header plus PDU) per datagram, no connection.

## Syntax

```scheme
(server (mode udp) (port 1502))
```

## Default

`port 1502`, `bind "127.0.0.1"`

## Valid in

- `port`
- `bind`
- `seed`

## Behaviour

- Unit routing, PDU handlers, the unknown-unit exception `0B` (or the [any-unit](../config/device/any-unit.md) device) and fault injection are the same as in
  [`tcp`](transport-tcp.md). The MBAP header is echoed except for the length.
- The reply is one datagram sent to the request's source address, so several peers can talk to the
  same socket and each gets its own answer.
- A datagram is ignored (no response, `DEBUG` log) when it is empty, longer than 260 bytes, has a
  protocol id other than 0, or its MBAP length does not equal the datagram size minus 6.
- UDP gives no ordering or delivery guarantee and the simulator never retransmits. The `drop` and
  `delay` faults model loss and latency on top of it. A delayed response waits in a fixed 16-slot
  table and is dropped with a `WARN` if the table is full.
- `disconnect` and `corrupt` mode `checksum` are rejected at load time. There is no TLS (no DTLS)
  and no idle timeout.
- RTU and ASCII frames over UDP are the separate [`rtu-udp`](transport-rtu-udp.md) and
  [`ascii-udp`](transport-ascii-udp.md) modes.

## Errors

| Message | Meaning |
|---|---|
| load error | `tls`, `idle-timeout-ms`, serial or RTU timing options, `broadcast-execute`, `disconnect`, `checksum` corruption |

## Examples

```bash
./mogbus profiles/modbus-udp.scm
```

## See also

- [Profile modbus-udp.scm](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/modbus-udp.scm)
- [Modbus TCP](transport-tcp.md)
