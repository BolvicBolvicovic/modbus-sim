# `(holding (count ...))`

Repeats a point `N` times at consecutive addresses.

## Syntax

```scheme
(count N)
```

## Default

`1`

## Valid in

- `(coil ...)`
- `(discrete ...)`
- `(input ...)`
- `(holding ...)`

## Behaviour

The step is the point's width: 1 for 16-bit types, 2 for 32-bit types, 4 for 64-bit types, `length`
for strings. All copies get the same value, ramp or model (a lag target is not shifted). A device can
have at most 8192 points total (`DEVICE_MAX_POINTS`); a count that would exceed this is rejected.

## Examples

```scheme
(device (unit-id 1)
  (coil 0 (count 16))                          ; coils 0..15, all off
  (holding 10 (type u32) (count 3) (static 7))) ; addresses 10-11, 12-13, 14-15
```

## See also

- [point](point.md)
