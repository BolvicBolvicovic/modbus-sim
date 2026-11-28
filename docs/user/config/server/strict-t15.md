# `(server (strict-t15 ...))`

Turns on a best-effort t1.5 discard heuristic for RTU byte streams.

## Syntax

```scheme
(strict-t15 on|off)
```

## Default

`off`

## Valid in

- `rtu-pty`
- `rtu-device`

## Behaviour

Opt-in, in baud-derived auto mode only. [char-timing](char-timing.md) replaces it with real
character-level timing and wins when both are set. It is never applied to RTU-over-TCP, because TCP `read()`
boundaries do not reflect UART character timing.

## Examples

```scheme
(server (mode rtu-pty) (baud 9600) (strict-t15 on))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [baud](baud.md)
- [char-timing](char-timing.md)
- [serial framing](framing.md)
