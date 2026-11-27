# `(static VALUE)`

Gives a point a value that never changes by itself.

## Syntax

```scheme
(static VALUE)
```

## Default

`0` (or off for bit tables) when a point has no behaviour

## Valid in

- `(coil ...)`
- `(discrete ...)`
- `(input ...)`
- `(holding ...)`

## Behaviour

Bit tables take `on` or `off`; registers take a number (see [type](../points/type.md) for ranges and
non-finite values, and [strings](../points/string.md) for text).

## Errors

- `a point can have only one of (static ...), (ramp ...), (sine ...), (lag ...) and (clock ...)`: choose one.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (static 230))
  (input 2 (type float32) (static 0.97))
  (coil 0 (static on)))
```

## See also

- [ramp](ramp.md)
- [client writes](client-writes.md)
