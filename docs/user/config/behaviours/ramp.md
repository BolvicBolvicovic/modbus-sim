# `(ramp ...)`

Moves a value from `from` towards `to`, by `step`, once every `every-ms` milliseconds.

## Syntax

```scheme
(ramp (from N) (to N) [(step N)] [(every-ms N)] [(mode wrap|bounce|hold)])
```

## Default

`step 1`, `every-ms 1000`, `mode wrap`

## Valid in

- `(input ...)`
- `(holding ...)`

## Behaviour

| Entry | Meaning | Default |
|---|---|---|
| `(from N)` | first value | required |
| `(to N)` | last value | required |
| `(step N)` | how much it changes each time. Always positive; the direction follows from `from` and `to` | `1` |
| `(every-ms N)` | milliseconds between steps, 1-86400000 | `1000` |
| `(mode ...)` | what happens at the end | `wrap` |

Modes:

- `wrap`: jumps back to `from` and starts again (a sawtooth). `0 10 20 30 0 10 ...`
- `bounce`: turns around and walks back (a triangle). `0 10 20 30 20 10 0 10 ...`
- `hold`: stays on the last value. `0 10 20 30 30 30 ...`

The value never goes beyond `to`. If the step does not divide the range evenly, the last value is the
last one that fits: from 0 to 25 by 10 gives `0 10 20`.

Ramps start when the simulator starts, and every ramp in the file starts at the same moment. Ramps are
not available for coils and discrete inputs.

## Errors

- `missing required option (to ...)`: a ramp needs `from` and `to`.
- `ramp step must be greater than 0`: `step` is always positive; use `from` and `to` for the direction.
- `ramp range is too large for the step`: the ramp would need 2^63 steps or more; increase `step` or narrow `from`/`to`.

## Examples

```scheme
(device (unit-id 1)
  (input 0 (type s16)
    (ramp (from 180) (to 240) (step 2) (every-ms 3000) (mode bounce))))
```

Watch it move:

```bash
./tools/modbus_probe.py --watch 1 input 0 1 --as s16
```

## See also

- [sine](sine.md)
- [noise](noise.md)
