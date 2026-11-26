# Modbus ASCII over UDP (`ascii-udp`)

Modbus ASCII frames, exactly one per UDP datagram, no connection.

## Syntax

```scheme
(server (mode ascii-udp) (port 1502))
```

## Default

`port 1502`, `bind "127.0.0.1"`

## Valid in

- `port`
- `bind`
- `seed`

## Behaviour

- Each datagram must be exactly one complete `:`...`CR LF` frame. There is no stream, so no
  reassembly, no resynchronization on a second colon, no partial-frame timeout and no tolerated
  inter-frame whitespace.
- A datagram without `CR LF`, with bytes before the colon or after the `LF`, with two frames, with an
  odd or invalid hex count or with a bad LRC is ignored (no response, never reaches PDU dispatch) and
  counts one FC 08 `0x000C` error on every device, as in [`ascii-tcp`](transport-ascii-tcp.md).
- Empty datagrams and those longer than 515 bytes (the largest ASCII frame) are dropped before
  decoding, without counting.
- Lowercase hex is accepted on receive; replies are uppercase, one frame per datagram, sent to the
  request's source address.
- Unit 0 (broadcast) and unknown units get no response and never execute. Unit ids are 1 to 247.
  [`0x0003`](../modbus/diagnostics/sub-0003-change-ascii-delimiter.md) is refused with exception 03.
- Faults behave as in `ascii-tcp`: `corrupt` mode `checksum` damages the reply LRC, and delayed
  responses use the shared 16-slot table (dropped with a `WARN` when full). `disconnect` is rejected
  at load time.

## Errors

| Message | Meaning |
|---|---|
| load error | `tls`, `idle-timeout-ms`, serial line options, `broadcast-execute` or `disconnect` |

## Examples

```bash
./mogbus profiles/ascii-udp.scm
```

## See also

- [Profile ascii-udp.scm](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/ascii-udp.scm)
- [Modbus ASCII](transport-ascii.md)
