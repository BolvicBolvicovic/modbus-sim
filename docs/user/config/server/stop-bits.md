# `(server (stop-bits ...))`

Sets the number of tty stop bits of a serial line.

## Syntax

```scheme
(stop-bits 1|2)
```

## Default

`1`

## Valid in

- `rtu-pty`
- `rtu-device`
- `ascii-pty`
- `ascii-device`

## Examples

```scheme
(server (mode rtu-pty) (parity none) (stop-bits 2))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [parity](parity.md)
- [data-bits](data-bits.md)
