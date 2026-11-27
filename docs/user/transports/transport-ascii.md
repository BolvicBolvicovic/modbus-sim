# Modbus ASCII (`ascii-pty`, `ascii-device`)

Modbus ASCII frames over a byte stream: a new Linux pseudo-terminal or an existing serial-like device.

## Syntax

```scheme
(server (mode ascii-pty) (baud 9600))
(server (mode ascii-device) (path "/dev/ttyUSB1"))
(server (mode ascii-pty) (data-bits 7) (parity even) (stop-bits 1))   ; 7E1 line
```

## Default

raw 8N1

## Valid in

- `ascii-pty`
- `ascii-device` (adds `path`)
- `seed`
- `baud`
- `data-bits`
- `parity`
- `stop-bits`

## Behaviour

Choose a PTY or an existing device, never both in one config. ASCII uses the same PDU handlers and
device model as the other transports but is not Modbus TCP (MBAP). RTU timing options and
`broadcast-execute` are rejected in ASCII modes.

### Frame format

`:` + uppercase hex digits for `[addr][PDU...][LRC]` + `CR LF`. `addr` is one byte, the PDU is 1 to
253 bytes, and the LRC is `(u8)(0 - sum(addr through PDU))`. A valid frame therefore has an even
number of hex digits. Responses use uppercase hex; received frames may use upper or lowercase. The
largest encoded frame is `1 + 2 + 2*253 + 2 + 2 = 513` bytes. A `(fault (kind corrupt) (mode checksum))`
response carries a deliberately damaged LRC; `pdu` and `register` corruption leave the recomputed
LRC valid.

### Framing

A frame is complete as soon as `CR LF` arrives. There is no t3.5, t1.5, `frame-gap-ms` or `strict-t15`
behaviour for ASCII.

- Complete frames are dispatched immediately, including several in one `read()`.
- A bounded receive buffer accumulates partial frames across reads. A trailing `CR` waits for its `LF`
  in a later read.
- A second `:` before the terminator starts a new frame: the bytes up to that colon are discarded and
  the rest is rescanned, so a truncated frame followed by a valid request still delivers the request.
- Bytes before a first colon that are only `CR`, `LF` or space are ignored as inter-frame whitespace;
  other garbage there is discarded.
- A buffered frame that gets no new byte for 1 s is dropped, so it cannot corrupt a later request.

Every discarded or undecodable frame (bad framing, invalid hex, bad LRC or a timed-out partial frame)
increments the FC 08 [`0x000C` counter](../modbus/diagnostics/sub-000c-bus-communication-error-count.md) on every device. Inter-frame
`CR`, `LF` and space are not errors, and neither are frames that decoded and were answered with an
exception.

### No response, no dispatch

- **Malformed framing**: a missing leading `:`, a missing or wrong `CR LF`, an odd hex count, an
  invalid hex digit, an overlong frame, a `CR` followed by a non-`LF`, or a lone `LF` inside a frame.
- **Bad LRC**: the decoded LRC does not match the sum of the address and PDU bytes.
- **Unknown unit**: no device has that unit id (and no device has [`(any-unit on)`](../config/device/any-unit.md) with the address in 1-247).
- **Broadcast (unit 0)**: ignored in every ASCII mode, never executed.
- **Fault drop or disconnect**: the fault suppresses the response; a disconnect also closes a
  TCP-like connection (never a serial fd).

Delay faults hold the response without blocking other work. Unit ids must be 1 to 247 (`--check`
applies the same rule). [`0x0003` Change ASCII Delimiter](../modbus/diagnostics/sub-0003-change-ascii-delimiter.md) is refused
with exception 03: the framing stays `CR LF`.

### The line

`(baud N)` may set the serial line or PTY speed but never affects ASCII framing or timing. When the fd
is a tty, mogbus configures raw 8N1 (8 data bits, no parity, 1 stop bit, no flow control). The
traditional Modbus ASCII 7E1 line is opt-in: `(data-bits 7) (parity even) (stop-bits 1)` sets `CS7`,
even parity and 1 stop bit. Serial line settings are accepted only in the PTY and device modes.

## Errors

| Message | Meaning |
|---|---|
| `serial ports are not supported on Windows yet` | device modes on the native Windows build |
| exit status 1 | the device or PTY could not be opened |

## See also

- [Modbus ASCII over TCP](transport-ascii-tcp.md)
- [Modbus ASCII over UDP](transport-ascii-udp.md)
- [Modbus RTU](transport-rtu.md)
