# `(server (mode ...))`

Selects the transport the server speaks: Modbus TCP, UDP, RTU or ASCII, on a socket, a PTY or a serial device.

## Syntax

```scheme
(mode tcp|udp|rtu-pty|rtu-device|rtu-tcp|rtu-udp|ascii-pty|ascii-device|ascii-tcp|ascii-udp)
```

## Default

`tcp`

## Valid in

- `(server ...)`

## Behaviour

There are ten modes. Device modes require `(path "PATH")`; the others forbid it.

| Mode | Transport | `(path ...)` |
|---|---|---|
| `tcp` | Modbus TCP (MBAP) on a TCP listen socket, optionally wrapped in TLS | forbidden |
| `rtu-pty` | Modbus RTU on a new Linux PTY (slave path is printed at startup) | forbidden |
| `rtu-device` | Modbus RTU on an existing byte-stream device | required |
| `rtu-tcp` | Modbus RTU frames on a TCP listen socket (not MBAP) | forbidden |
| `ascii-pty` | Modbus ASCII on a new Linux PTY (slave path is printed at startup) | forbidden |
| `ascii-device` | Modbus ASCII on an existing byte-stream device | required |
| `ascii-tcp` | Modbus ASCII frames on a TCP listen socket (not MBAP) | forbidden |
| `udp` | Modbus UDP: one MBAP ADU per datagram on a UDP socket | forbidden |
| `ascii-udp` | Modbus ASCII: one `:`...CR LF frame per datagram on a UDP socket | forbidden |
| `rtu-udp` | Modbus RTU frames, one per datagram, on a UDP socket (not MBAP) | forbidden |

For how RTU and ASCII frames are delimited, see [serial framing](framing.md).

### Datagram modes

In the three UDP modes replies go to the source address of the request, and there are no
connections. Each accepts only `mode`, `port`, `bind` and `seed` (`rtu-udp` also
[`broadcast-execute`](broadcast-execute.md)); `tls`, `idle-timeout-ms` (there are no connections),
serial line options and RTU timing options are rejected at load time.

- `udp`: unit ids 0-255 are allowed as in `tcp`. A datagram must hold exactly one MBAP ADU; others
  (bad protocol id, length mismatch, empty, longer than 260 bytes) are ignored.
- `ascii-udp`: unit ids follow the RTU/ASCII rule (1-247; 0 is broadcast and never answered). A
  datagram must hold exactly one complete frame; anything else (no CR LF, extra bytes, two frames,
  bad LRC) is ignored and counted as an FC08 `0x000C` communication error on every device, and empty
  or longer than 515 byte datagrams are dropped without counting.
- `rtu-udp`: `baud`, `frame-gap-ms` and `strict-t15` are rejected because datagram boundaries delimit
  frames, so there is no t3.5/t1.5 timing. Unit ids follow the `rtu-tcp` rule (1-247). A datagram must
  hold exactly one valid RTU frame (CRC checked); otherwise it is ignored.

## Examples

```scheme
(server (mode tcp) (port 1502) (bind "127.0.0.1") (seed 1))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
(server (mode rtu-pty) (baud 9600) (frame-gap-ms 20)
        (parity even) (stop-bits 2) (strict-t15 on))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
(server (mode rtu-device) (path "/dev/ttyUSB0") (baud 19200) (broadcast-execute on))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
(server (mode rtu-tcp) (port 1502) (bind "0.0.0.0") (baud 9600)
        (idle-timeout-ms 5000) (broadcast-execute on))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
(server (mode ascii-pty) (data-bits 7) (parity even) (stop-bits 1)) ; 7E1 line
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
(server (mode ascii-tcp) (port 1502) (bind "127.0.0.1") (idle-timeout-ms 5000))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
; Modbus UDP: one MBAP ADU per datagram; replies go to the sender's address
(server (mode udp) (port 1502) (bind "127.0.0.1"))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
; Modbus ASCII over UDP: one :...CR LF frame per datagram
(server (mode ascii-udp) (port 1502) (bind "127.0.0.1"))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
; Modbus RTU frames, one per UDP datagram
(server (mode rtu-udp) (port 1502) (bind "127.0.0.1") (broadcast-execute off))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
(server (mode ascii-device) (path "/dev/ttyUSB1"))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [server](server.md)
- [serial framing](framing.md)
- [path](path.md)
- [Modbus support](../../modbus-support.md)
