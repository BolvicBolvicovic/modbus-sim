# `(fault (kind drop))`

Never sends the response, so the client sees a timeout.

## Syntax

```scheme
(fault (kind drop) [(percent N)])
```

## Default

`percent 100`

## Valid in

- `(device ...)`
- every mode

## Behaviour

The write or read itself **still happens** on the device, like a real device whose answer got lost. A
plain `drop` only suppresses the response and leaves the connection open; see
[disconnect](disconnect.md) to close it as well.

## Examples

```scheme
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind drop) (percent 20)))   ; 1 in 5 answers never arrives
```

## See also

- [disconnect](disconnect.md)
- [delay](delay.md)
