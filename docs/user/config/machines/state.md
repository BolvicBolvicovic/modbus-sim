# `(machine (state ...))`

Declares a state with optional enter, tick and exit handlers.

## Syntax

```scheme
(state NAME
  [(on-enter STATEMENT ...)]
  [(on-tick STATEMENT ...)]
  [(on-exit STATEMENT ...)]
  [(fault (kind K) ...)])
```

## Default

none

## Valid in

- `(machine ...)`

## Behaviour

One or more per machine (at most 16), before any `(transition ...)`. Handlers are optional and may
appear in any order within a state; each can appear at most once per state.

- `on-enter` runs when the state is entered, after the state register is written.
- `on-tick` runs once per request-time tick while no transition fired.
- `on-exit` runs when a [transition](transition.md) leaves the state.

See [transition](transition.md) for how catching up through several due transitions affects `on-tick`.

## Examples

```scheme
(device (unit-id 1)
  (coil 0)
  (machine m
    (initial a)
    (state a (on-enter (set! (coil 0) #t)) (on-exit (set! (coil 0) #f)))
    (state b)
    (transition a b (after-ms 1000))))
```

## See also

- [transition](transition.md)
- [statements](statements.md)
