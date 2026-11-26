# `(server (parity ...))`

Sets the tty parity of a serial line.

## Syntax

```scheme
(parity none|even|odd|mark|space)
```

## Default

`none`

## Valid in

- `rtu-pty`
- `rtu-device`
- `ascii-pty`
- `ascii-device`

## Behaviour

`mark` and `space` require `CMSPAR` support.

## Examples

```scheme
(server (mode rtu-pty) (parity even) (stop-bits 1))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [data-bits](data-bits.md)
- [stop-bits](stop-bits.md)
