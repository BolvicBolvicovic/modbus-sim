# `(server (char-timing ...))`

Models UART character timing on RTU byte streams: t1.5 and t3.5 are measured between characters, and replies leave at the line rate.

## Syntax

```scheme
(char-timing on|off)
```

## Default

`off`

## Valid in

- `rtu-pty` (needs `(baud N)`)
- `rtu-device` (the baud is `(baud N)` or the tty's current rate)

## Behaviour

Without it, RTU frame boundaries come from `read()` chunks, which hide the byte timing of a real
serial line. With it, mogbus dates every received character and applies the Modbus rules between
characters:

- A silence of more than **t1.5** inside a frame discards the frame, and every character is dropped
  until the line has been silent for t3.5.
- A silence of **t3.5** ends a frame (an incomplete one is discarded, and counted as a bus
  communication error when it was a bad frame, as always).
- A character time is 11 bits, t1.5 and t3.5 follow [baud](baud.md) (fixed 750 us and 1750 us above
  19200 baud), and [frame-gap-ms](frame-gap-ms.md) still overrides t3.5. t1.5 always comes from the
  baud. It supersedes [strict-t15](strict-t15.md): both may be set, only `char-timing` is used.
- The frame is processed after the t3.5 silence, so a reply never starts sooner than t3.5 after the
  request's last character, as on a real bus. Incomplete frames are not kept for one more gap as in
  the default baud-derived mode.

**Reply pacing.** When the handle is not a real serial port (a PTY, or a host fd in the library),
the reply is also sent at the line rate: one character every 11 bit times, so a 45-byte reply at
9600 baud takes about 52 ms, as on the wire. It is scheduled through the poll timeout, so other
servers keep running. On a real serial port the UART already takes that time, so the reply is
written at once, and mogbus asks the driver for low-latency mode (Linux `ASYNC_LOW_LATENCY`) so
received characters reach it sooner; a refusal is logged at warning level and ignored.

### How the characters are dated

A user-space program cannot see when each character arrived; it sees when `read()` returned. The
newest character of a chunk is dated at the read, and each earlier one a character time before it
(never closer than one character time to its predecessor, since the line carries one character at a
time). What this achieves:

- **Real serial port**: the date is as good as the driver's delivery. A UART FIFO or a USB adapter
  (its 1 to 16 ms latency timer) hands over characters in bursts, and a gap shorter than that
  latency cannot be told from a burst. Above about 19200 baud, where t1.5 is 750 us, the result is
  only reliable on a built-in UART with low-latency mode. Linux does not guarantee character timing
  to user space.
- **PTY or host fd**: a PTY has no line timing at all. The model sees the timing of what the peer
  writes: a peer that writes a whole frame in one `write()` looks like a frame at line rate, a peer
  that pauses between writes shows that pause. The estimate is accurate to about 0.1 to 0.5 ms on
  an idle machine (poll wake-up plus the peer's own sleep overshoot), which makes tests of
  byte-time-sensitive clients reproducible at 1200 baud and below and usable at 9600 baud.
- **Reply pacing** is exact over a whole reply (the last character is never earlier than its line
  time and about 1 to 2 ms later at most), but the poll timeout has millisecond resolution: above
  about 19200 baud several characters go out in each wake-up, so the spacing between characters is
  not reproduced.

## Errors

- `char-timing needs (baud N) in rtu-pty mode`: a PTY has no baud to read back, so it must be given.
- `char-timing is not valid in rtu-tcp mode`: RTU-over-TCP and RTU-over-UDP are separate transports
  without UART timing; ASCII has none either.
- `RTU: character timing needs a known baud rate`: an `rtu-device` whose tty reports no baud, at
  start-up.

## Examples

```scheme
(server (mode rtu-pty) (baud 9600) (char-timing on))
(device (unit-id 1) (holding 0 (static 1)))
```

```scheme
(server (mode rtu-device) (path "/dev/ttyUSB0") (char-timing on))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [baud](baud.md)
- [frame-gap-ms](frame-gap-ms.md)
- [strict-t15](strict-t15.md)
- [serial framing](framing.md)
