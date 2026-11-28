# Serial framing (RTU and ASCII)

How RTU and ASCII frames are delimited and what is dropped, in every mode that carries them.

## Behaviour

### RTU

RTU frame boundaries use baud-derived t3.5 when baud is known (`(baud N)` or the tty's current rate),
unless `(frame-gap-ms N)` overrides it; with no known baud the fallback is 20 ms. `strict-t15` is a
best-effort RTU byte-stream-only heuristic in baud-derived auto mode and never applies to
RTU-over-TCP. `char-timing` replaces both with t1.5 and t3.5 measured between characters (see
[char-timing](char-timing.md) for what that can and cannot achieve).

### ASCII

ASCII is `:` + hex `[addr][PDU][LRC]` + `CR LF`, delimited by the terminator rather than silence; it
has no RTU timing options.

- Inter-frame `CR`, `LF`, and spaces are ignored.
- A trailing `CR` waits for `LF`.
- A second `:` resynchronizes.
- A partial frame with no byte for 1 s is dropped.
- Rejected or undecodable frames increment FC08 `0x000C` on every device.
- Serial ASCII ttys default to 8N1; `(data-bits 7) (parity even) (stop-bits 1)` selects 7E1 but does
  not change framing.
- `ascii-tcp` carries the same frames over TCP and accepts only `mode`, `port`, `bind`, `seed`, and
  `idle-timeout-ms`.
- All ASCII modes ignore unit-0 broadcasts and reject `broadcast-execute`.

## See also

- [mode](mode.md)
- [baud](baud.md)
- [frame-gap-ms](frame-gap-ms.md)
- [strict-t15](strict-t15.md)
- [char-timing](char-timing.md)
- [broadcast-execute](broadcast-execute.md)
