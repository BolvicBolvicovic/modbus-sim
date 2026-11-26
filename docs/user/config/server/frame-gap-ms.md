# `(server (frame-gap-ms ...))`

Overrides the RTU inter-frame silence, in milliseconds.

## Syntax

```scheme
(frame-gap-ms N)
```

## Default

`0` (auto)

## Valid in

- `rtu-pty`
- `rtu-device`
- `rtu-tcp`

## Behaviour

`N` is 1-60000. It takes precedence over the baud-derived t3.5 timing. If it is omitted and the baud
is unknown, RTU falls back to 20 ms.

## Examples

```scheme
(server (mode rtu-pty) (frame-gap-ms 20))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [baud](baud.md)
- [serial framing](framing.md)
