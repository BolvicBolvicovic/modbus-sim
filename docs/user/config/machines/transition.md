# `(machine (transition ...))`

Moves a machine from one state to another when a condition holds or a deadline passes.

## Syntax

```scheme
(transition FROM TO (when EXPR) (after-ms MS))
(transition FROM TO (after-ms (random MIN MAX)))
```

## Default

none

## Valid in

- `(machine ...)`

## Behaviour

Each transition has at least one trigger and at most one `(when EXPR)` and one `(after-ms ...)`.
At most 32 per machine.

`MS` is a whole number from 1 to 86400000 (24 hours). `(random MIN MAX)` needs whole-number bounds with
`1 <= MIN <= MAX <= 86400000`; the deadline is drawn from that inclusive range, using the
[seed](../server/seed.md). An `after-ms` transition with no `when` fires at its deadline; a `when`
transition fires at the current time, gated by its optional deadline.

Due after-only transitions win, earliest deadline first (ties in declaration order), then `when`
transitions are tested in declaration order; the first match fires. Firing runs the old state's
`on-exit`, switches state, redraws all deadlines from the firing time, then runs the new state's
`on-enter`. If no transition fires, one `on-tick` runs. A machine fires at most 64 transitions per
tick. When one tick catches up through several due transitions, intermediate states get
`on-exit`/`on-enter` but no `on-tick`; only the final state gets one `on-tick` at request time (none if
the 64-firing cap is reached). Ramps or accumulators that need to advance through skipped states should
update in `on-exit` using `elapsed-ms` instead.

## Examples

```scheme
(device (unit-id 1)
  (coil 0)
  (machine m
    (initial idle)
    (state idle)
    (state running)
    (state tripped)
    (transition idle running (when (coil 0)))
    (transition running tripped (after-ms (random 60000 600000)))
    (transition tripped idle (after-ms 5000))))
```

## See also

- [state](state.md)
- [expressions](expressions.md)
