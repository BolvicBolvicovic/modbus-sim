# `(machine (initial ...))`

Names the state a machine starts in.

## Syntax

```scheme
(initial STATE)
```

## Default

none (required)

## Valid in

- `(machine ...)`

## Behaviour

`STATE` must name a state of the machine. The initial state's `on-enter` runs at simulator start time
and after a [restart](../faults/restart.md).

## Examples

```scheme
(device (unit-id 1)
  (machine m (initial idle) (state idle)))
```

## See also

- [state](state.md)
