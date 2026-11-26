# `(server (seed ...))`

Seeds the random dice, so a run can be repeated exactly.

## Syntax

```scheme
(seed N)
```

## Default

`0` (a fixed built-in seed)

## Valid in

- all modes

## Behaviour

`N` is 0-4294967295. The seed drives the `percent`-based [faults](../faults/percent.md) and the
`(after-ms (random MIN MAX))` deadlines of [machines](../machines/transition.md). The same seed gives
the same sequences. Schedules are deterministic and do not use the dice. With no `(server ...)` form
the seed is 0, and the position of `(server ...)` in the file does not matter.

## Examples

```scheme
(server (mode tcp) (seed 42))
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind drop) (percent 20)))   ; the same 1 in 5 answers are lost on every run
```

## See also

- [percent](../faults/percent.md)
- [transition](../machines/transition.md)
