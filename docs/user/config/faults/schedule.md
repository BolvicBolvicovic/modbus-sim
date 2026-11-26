# `(fault (schedule ...))`

Fires the fault on a fixed, non-repeating timeline instead of by chance.

## Syntax

```scheme
(schedule (step (after-ms N) (count N)) ...)
```

## Default

none (a [`percent`](percent.md) roll)

## Valid in

- `(fault ...)`

## Behaviour

At most 8 steps; each step needs `(after-ms N)` (0-86400000) and `(count N)` (1-1000000). Not allowed
together with `percent`.

The first step becomes active `after-ms` after device start (or, for a machine
[state overlay](../machines/state-fault.md), after that overlay became active). Each later step
becomes active `after-ms` after the request that completes the previous step. A step fires exactly
`count` consecutive requests, then the next step waits; after the last step the schedule stops.
Schedules never repeat.

Progress restarts when the active fault definition or schedule changes (for example a machine state
overlay becoming active) and on a device restart, except that the [`restart`](restart.md) kind keeps
its own progress so a scheduled restart does not re-arm itself. A different source counts as a change
even with an identical schedule: switching between the device definition and a state overlay, between
two overlays, or re-entering a state restarts progress. Requests that produce no response
(broadcast-execute unit-0 requests, listen-only mode) do not consume schedule steps. Schedules are
deterministic and do not use the random dice.

## Errors

- `percent cannot be combined with schedule`: choose a fixed percent chance or a schedule, not both.
- `schedule has more than 8 steps`: shorten the schedule.
- `schedule needs at least one (step ...)`: add a step.
- `expected (step (after-ms N) (count N))`: each schedule entry must be a `(step ...)` form.
- `schedule step: missing required option (after-ms ...)`: every step needs both `after-ms` and `count`.
- `(after-ms ...): must be between 0 and 86400000`: fix the step delay.
- `(count ...): must be between 1 and 1000000`: fix the number of requests per step.

## Examples

```scheme
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind sensor-disconnect)
    (schedule (step (after-ms 5000) (count 600)))))   ; 5 s in, 600 requests fail, then recover
```

## See also

- [percent](percent.md)
- [restart](restart.md)
