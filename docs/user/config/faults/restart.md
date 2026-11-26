# `(fault (kind restart))`

Restarts the device after the response is built, like a power cycle.

## Syntax

```scheme
(fault (kind restart) [(percent N)] [(schedule ...)])
```

## Default

`percent 100`

## Valid in

- `(device ...)`
- every mode

## Behaviour

`restart` runs after the request is processed and the response is built, and does not replace the
response; it still happens when that response is dropped or delayed. It:

- resets configured points to their configured kinds and initial values (undoing client writes),
  restarts ramp and sine timelines, and resets lags to their `initial` value;
- resets all machines to their initial states, variables and timers and runs the initial `on-enter`
  (state registers included);
- repairs [degraded](../behaviours/degradation.md) sensors, since their timeline starts again;
- re-arms fault schedules from the restart time. The restart kind keeps its own schedule progress, so
  a scheduled restart advances through its steps and stops instead of re-arming itself forever.

It does not clear FC 08/FC 11 diagnostic counters, the FC 12 event log, file records, FIFO contents,
listen-only mode, or the RNG seed.

## Examples

```scheme
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind restart)                       ; one restart on the first request after 60 s
    (schedule (step (after-ms 60000) (count 1)))))
```

## See also

- [schedule](schedule.md)
- [client writes](../behaviours/client-writes.md)
