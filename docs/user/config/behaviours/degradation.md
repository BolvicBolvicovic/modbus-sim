# `(degradation ...)`

Ages a sensor: its value drifts, gets noisier, sticks and finally fails.

## Syntax

```scheme
(degradation
  [(after-ms N)]
  [(drift-per-hour D)]
  [(noise-growth-per-hour G)]
  [(stuck-after-ms S)]
  [(fail-after-ms F)])
```

## Default

`after-ms 0`; every effect is off until given

## Valid in

- `(input ...)`
- `(holding ...)`
- `(coil ...)` and `(discrete ...)` (`stuck-after-ms` and `fail-after-ms` only)

## Behaviour

It follows the behaviour (and the optional [`(noise ...)`](noise.md)) in the same point, which must
have a `static`, `ramp`, `sine` or `lag`. All times are milliseconds **since the device started**
(0-4294967295), so a restart, whether a [`restart` fault](../faults/restart.md) or FC 08 restart
communications, repairs the sensor and the timeline begins again.

| Entry | Meaning | Default |
|---|---|---|
| `(after-ms N)` | drift and noise growth start at N ms; before that the point is healthy | `0` |
| `(drift-per-hour D)` | adds `D * (t - after) / 1 hour` to the value once `t > after`; any finite number, negative drifts down. Registers only | none |
| `(noise-growth-per-hour G)` | adds `G * (t - after) / 1 hour` to the noise amplitude (G is 0 or more). The point's own `(noise ...)` keeps its every-ms and distribution; without one the extra noise is uniform with a new sample every 1000 ms. Registers only | none |
| `(stuck-after-ms S)` | from S on the value stays at what it was (drift and noise included) at the first evaluation at or after S, whatever the base behaviour does afterwards | none |
| `(fail-after-ms F)` | from F on, reads that cover an address of this point answer exception 04 | none |

At least one of `drift-per-hour`, `noise-growth-per-hour`, `stuck-after-ms` and `fail-after-ms` is
required. `stuck-after-ms` and `fail-after-ms` also work on `coil` and `discrete` points (which must be
`(static on/off)`); drift and noise growth are for registers only. Order in the output: base value,
drift, noise (with its growth), then stuck. The result goes through the point's `type` like any other
value.

A failed point only answers exception 04 to reads: FC 01, 02, 03, 04, 23 and a user-defined
`(registers ...)` action whose range covers it (the exception is checked after the address check, so an
undefined address still answers 02, and FC 23 does not perform its write when its read fails). Writes
are unaffected: a client can still write a failed holding point or coil. Failed reads count as
exceptions in the FC 08 diagnostic counters and in the metrics.

A client write, or an embedding host claiming the point, removes the model effects like for any
modeled point (see [client writes](client-writes.md)): drift, noise and stuck stop, and the written
value is returned. The failure does not stop: `fail-after-ms` stays configured until the device
restarts, so a written point that has failed keeps answering exception 04. Machines cannot drive a
point with `(degradation ...)`.

## Errors

- `(degradation ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to age`: degradation modifies a behaviour; add one.
- `degradation needs at least one of drift-per-hour, noise-growth-per-hour, stuck-after-ms and fail-after-ms`: `after-ms` alone has no effect; add an effect (a drift of 0 is none).
- `noise-growth-per-hour must be 0 or more`: noise cannot shrink; use a positive growth.
- `coils and discrete inputs only support stuck-after-ms and fail-after-ms`: drift and noise growth are for registers.
- `degradation numbers must be finite`: no NaN or infinity.
- `fail-after-ms, stuck-after-ms or after-ms out of range`: times are whole milliseconds, 0-4294967295.

## Examples

```scheme
(device (unit-id 1)
  (input 0 (type u16)
    (sine (offset 500) (amplitude 40) (period-ms 600000))
    (noise (amplitude 1))
    (degradation
      (after-ms 3600000)           ; healthy for the first hour
      (drift-per-hour 6)           ; then +6 per hour
      (noise-growth-per-hour 4)    ; and noisier by 4 per hour
      (stuck-after-ms 10800000)    ; frozen at 3 h
      (fail-after-ms 21600000)))   ; reads fail with exception 04 at 6 h
  (coil 0 (static on) (degradation (fail-after-ms 60000))))
```

See [`profiles/aging-sensor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/aging-sensor.scm).

## See also

- [noise](noise.md)
- [sensor-disconnect](../faults/sensor-disconnect.md)
