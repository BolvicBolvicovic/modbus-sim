# Modbus RTU over UDP (`rtu-udp`)

Modbus RTU frames (`[addr][PDU][CRC lo hi]`), exactly one per UDP datagram, no connection.

## Syntax

```scheme
(server (mode rtu-udp) (port 1502) (broadcast-execute on))
```

## Default

`port 1502`, `bind "127.0.0.1"`

## Valid in

- `port`
- `bind`
- `seed`
- `broadcast-execute`

## Behaviour

- The datagram boundary delimits the frame, so there is no t3.5 or t1.5 timing and `baud`,
  `frame-gap-ms` and `strict-t15` are rejected.
- Request semantics are those of [`rtu-tcp`](transport-rtu-tcp.md): a bad CRC, a frame shorter than 4
  bytes and an unknown unit (see [any-unit](../config/device/any-unit.md)) get no response; unit 0 is broadcast with no response and, with
  `(broadcast-execute on)`, executes on every device.
- The datagram must be exactly one frame. Two concatenated frames, trailing bytes, an empty datagram
  or one longer than 256 bytes are rejected whole, and nothing executes. Each rejected datagram of 4
  to 256 bytes counts as one FC 08 `0x000C` error on every device.
- The reply (a single RTU frame) goes to the request's source address. `delay` faults wait in the
  shared 16-slot table (dropped with a `WARN` when full).
- `disconnect` is rejected at load time. `corrupt` modes `pdu`, `register` and `checksum` all work
  (`checksum` flips the last CRC byte). Unit ids are 1 to 247.

## Errors

| Message | Meaning |
|---|---|
| load error | `baud`, `frame-gap-ms`, `strict-t15`, `tls`, `idle-timeout-ms` or `disconnect` |

## Examples

```bash
./mogbus profiles/rtu-udp.scm
```

## See also

- [Profile rtu-udp.scm](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/rtu-udp.scm)
- [Modbus RTU over TCP](transport-rtu-tcp.md)
- [Unit ids and broadcast](../modbus/unit-ids.md)
