# `(lag ...)`

A first-order lag: the value follows a goal and closes 63% of the remaining distance every `tau-ms`.

## Syntax

```scheme
(lag (target X) (tau-ms N) [(initial N)] [(gain N)] [(offset N)])
```

## Default

`initial` the goal, `gain 1`, `offset 0`

## Valid in

- `(input ...)`
- `(holding ...)`

## Behaviour

The value moves towards its goal `gain * target + offset` and closes `1 - 1/e` (63%) of the remaining
distance every `tau-ms`. It is the "heater following its setpoint" model. Registers only.

| Entry | Meaning | Default |
|---|---|---|
| `(target X)` | a number, or a point to follow: `(holding 1)`, `(input 4)`, `(coil 0)`, `(discrete 0)`. The point must be a configured point (first address); bits count as 0 or 1 | required |
| `(tau-ms N)` | time constant, 1-86400000 | required |
| `(initial N)` | value at start and after a restart | the goal |
| `(gain N)` | multiplier of the target | `1` |
| `(offset N)` | added to `gain * target` | `0` |

The target may be declared before or after the point, but not be the point itself. The value is
computed when a request arrives, with the exact solution over the time since the previous evaluation,
so it does not depend on how often clients read. The target is read at that moment: a target that
changes between two evaluations (for example a client writing the setpoint) is sampled at those times,
and every request, reads and writes alike, evaluates the lag first.

## Errors

- `lag target holding 9 is not the first address of a configured point`: a lag follows a configured point; declare it or fix the address.
- `a lag cannot follow itself`: point the `target` at another point.

## Examples

```scheme
(device (unit-id 1)
  (holding 1 (type s16) (static 300))     ; setpoint, writable
  (input 0 (type s16) (lag (target (holding 1)) (tau-ms 60000) (initial 200))))
```

Write a new setpoint:

```bash
./tools/modbus_probe.py write-reg 1 400   # the input at 0 now heads for 400
```

## See also

- [sine](sine.md)
- [client writes](client-writes.md)
