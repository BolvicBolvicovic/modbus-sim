# `(server (baud ...))`

Sets the serial speed, from which RTU derives its frame timing.

## Syntax

```scheme
(baud N)
```

## Default

`0` (unknown)

## Valid in

- `rtu-pty`
- `rtu-device`
- `rtu-tcp`
- `ascii-pty`
- `ascii-device`

## Behaviour

`N` is a standard termios baud rate (for example 9600, 19200, 38400, 115200). RTU derives t3.5
timing from it; in ASCII mode it only sets the serial line or PTY speed. If `baud` is omitted the
tty's current rate is used where there is one. See [serial framing](framing.md).

## Examples

```scheme
(server (mode rtu-pty) (baud 19200))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [frame-gap-ms](frame-gap-ms.md)
- [strict-t15](strict-t15.md)
- [char-timing](char-timing.md)
