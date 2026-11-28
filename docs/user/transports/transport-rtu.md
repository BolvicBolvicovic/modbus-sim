# Modbus RTU (`rtu-pty`, `rtu-device`)

Modbus RTU frames over a byte stream: a new Linux pseudo-terminal or an existing serial-like device.

## Syntax

```scheme
(server (mode rtu-pty) (baud 9600) (frame-gap-ms 20)
        (parity even) (stop-bits 2) (strict-t15 on))
(server (mode rtu-device) (path "/dev/ttyUSB0") (baud 19200))
```

## Default

8N1, baud taken from the tty (or unknown for a PTY), 20 ms frame gap when the baud is unknown

## Valid in

- `rtu-pty`
- `rtu-device` (adds `path`)
- `seed`
- `baud`
- `frame-gap-ms`
- `strict-t15`
- `char-timing`
- `broadcast-execute`
- `parity`
- `stop-bits`

## Behaviour

Choose a PTY or an existing device, never both in one config. `rtu-pty` creates a PTY and prints the
slave path at start-up; `rtu-device` needs `(path "PATH")`. The same PDU handlers and device model
are used as for TCP; only the framing differs.

### Frame format

`[addr][PDU...][CRC lo][CRC hi]`: the 2-byte Modbus CRC-16, low byte first. A valid frame has 1
address byte, 1 to 253 PDU bytes and 2 CRC bytes. A `(fault (kind corrupt) (mode checksum))`
response is sent with a deliberately wrong CRC, so the receiver must reject it; `pdu` and `register`
corruption leave the recomputed CRC valid.

### No response

- **Bad CRC**: frames whose CRC does not match are silently ignored.
- **Unknown unit**: a valid frame for a unit id with no device is ignored, unless a device has [`(any-unit on)`](../config/device/any-unit.md) and the address is 1-247.
- **Broadcast (unit 0)**: ignored by default. With `(server (broadcast-execute on))` the request
  runs on every configured device and still sends no response. See
  [Unit ids and broadcast](../modbus/unit-ids.md).

Unit ids must be 1 to 247. Bad frames count in the FC 08 [`0x000C` error counter](../modbus/diagnostics/sub-000c-bus-communication-error-count.md).

### The line

Device paths are opened with `O_NONBLOCK`, so start-up does not hang on a FIFO with no writer; the fd
stays non-blocking for the poll loop. When the fd is a tty, mogbus configures raw mode (`CS8`,
`CLOCAL|CREAD`, no flow control) with the selected parity and stop bits and preserves the existing
baud rate unless `(baud N)` is given. The default is 8N1.

`(parity none|even|odd|mark|space)` and `(stop-bits 1|2)` change the line format; `mark` and `space`
need `CMSPAR` support on the host. These apply only to ttys: non-tty fds skip termios setup.

### Frame boundaries

RTU uses inter-frame silence to find frame boundaries.

- With a known baud, t3.5 is 1.750 ms above 19200 baud and otherwise `ceil(3.5 * 11 / baud * 1,000,000)`
  microseconds. The baud is `(baud N)` (a standard termios rate) or, for a tty, the tty's current
  output baud. A `read()` gap of at least t3.5 ends a frame.
- `(frame-gap-ms N)` is an explicit override and wins over the baud-derived value.
- With no known baud (PTY or pipe) and no override, the default 20 ms is used. This is an
  approximation, because `read()` chunk boundaries do not show real UART character timing.
- `(strict-t15 on)` adds a best-effort t1.5 check in baud-derived auto mode: a `read()` gap between
  t1.5 and t3.5 inside an incomplete frame discards that partial frame. It is opt-in because
  `read()` boundaries can split valid frames on PTYs, real serial ports and other byte streams. It is
  never applied to RTU-over-TCP.
- `(char-timing on)` models the UART instead: every received character is dated, a silence over
  t1.5 inside a frame discards it and a silence of t3.5 ends it, and replies on a PTY or host fd are
  paced at the line rate (11 bits per character). It needs a known baud and replaces `strict-t15`.
  See [char-timing](../config/server/char-timing.md) for the precision a PTY or a real port gives.

## Errors

| Message | Meaning |
|---|---|
| `serial ports are not supported on Windows yet` | device modes on the native Windows build |
| exit status 1 | the device or PTY could not be opened |

## Examples

```bash
./mogbus profiles/power-meter.scm      # with (server (mode rtu-pty)) in the config
```

mogbus prints the slave path of the PTY; point your serial client at it.

## See also

- [char-timing](../config/server/char-timing.md)
- [Modbus RTU over TCP](transport-rtu-tcp.md)
- [Modbus RTU over UDP](transport-rtu-udp.md)
- [Unit ids and broadcast](../modbus/unit-ids.md)
- [Config reference: server](../config/server/index.md)
