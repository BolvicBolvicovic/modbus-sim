# Modbus RTU over TCP (`rtu-tcp`)

Modbus RTU frames carried on a TCP listen socket: no MBAP header, the frame is `[addr][PDU][CRC lo hi]`.

## Syntax

```scheme
(server (mode rtu-tcp) (port 1502) (bind "0.0.0.0") (baud 9600) (broadcast-execute on))
```

## Default

`port 1502`, `bind "127.0.0.1"`

## Valid in

- `port`
- `bind`
- `seed`
- `idle-timeout-ms`
- `baud`
- `frame-gap-ms`
- `broadcast-execute`

## Behaviour

- This is a separate transport from Modbus TCP (`tcp` still uses MBAP) and from RTU byte streams.
  It is an explicit listen mode, not a transparent gateway or proxy.
- The same PDU handlers, device model, unit routing, fault injection and CRC validation are used as
  for [RTU](transport-rtu.md). Up to 32 clients are served, and `idle-timeout-ms` closes idle connections.
- A bad CRC and an unknown unit (no exact match and no [any-unit](../config/device/any-unit.md) device) produce no response. Broadcast (unit 0) is ignored by default;
  `(broadcast-execute on)` executes it on every device with no response.
- A `(fault (kind disconnect) ...)` closes the TCP connection after the addressed frame is processed.
  It is not applied to bad-CRC, unknown-unit or broadcast frames, which stay silent without
  disconnecting.
- `baud` and `frame-gap-ms` select the inter-frame timing: an explicit `frame-gap-ms` wins, otherwise
  the baud-derived t3.5, otherwise the 20 ms default. t1.5 discard is never applied, because TCP
  `read()` boundaries do not reflect UART character timing and can split valid pipelined requests.
- Serial parity and stop bits do not apply (there is no tty) and are rejected in this mode.
- Unit ids must be 1 to 247.

## Errors

| Message | Meaning |
|---|---|
| load error | `parity`, `stop-bits`, `strict-t15`, `char-timing`, `data-bits`, `path` or `tls` in this mode |

## Examples

```scheme
(server (mode rtu-tcp) (port 1502) (bind "0.0.0.0") (baud 9600) (broadcast-execute on))
(device (name "RTU over TCP") (unit-id 1) (holding 0 (static 42)))
```

## See also

- [Modbus RTU](transport-rtu.md)
- [Modbus RTU over UDP](transport-rtu-udp.md)
- [Serial-to-TCP bridge on Windows](../windows/windows-serial.md)
