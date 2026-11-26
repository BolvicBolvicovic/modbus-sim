# `(server (data-bits ...))`

Sets the number of tty data bits of a serial ASCII line.

## Syntax

```scheme
(data-bits 7|8)
```

## Default

`8`

## Valid in

- `ascii-pty`
- `ascii-device`

## Behaviour

ASCII ttys default to 8N1. `(data-bits 7)` with `(parity even)` selects traditional 7E1 Modbus ASCII;
this does not change the framing.

## Examples

```scheme
(server (mode ascii-pty) (data-bits 7) (parity even) (stop-bits 1))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [parity](parity.md)
- [stop-bits](stop-bits.md)
