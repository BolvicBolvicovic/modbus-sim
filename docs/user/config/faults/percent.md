# `(fault (percent ...))`

Sets the chance, per response, that the fault fires.

## Syntax

```scheme
(percent N)
```

## Default

`100`

## Valid in

- `(fault ...)`

## Behaviour

`N` is 0-100. It cannot be combined with [`schedule`](schedule.md). The dice follow
[`(seed N)`](../server/seed.md), so the same seed repeats the same pattern.

## Examples

```scheme
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind drop) (percent 20)))
```

## See also

- [schedule](schedule.md)
- [seed](../server/seed.md)
