# `(sine ...)`

Makes a register follow a sine wave of the time since the simulator started.

## Syntax

```scheme
(sine (offset N) (amplitude N) (period-ms N) [(phase-deg N)])
```

## Default

`phase-deg 0`

## Valid in

- `(input ...)`
- `(holding ...)`

## Behaviour

The value is `offset + amplitude * sin(2 pi * t / period + phase)`. Registers only.

| Entry | Meaning | Default |
|---|---|---|
| `(offset N)` | centre value | required |
| `(amplitude N)` | peak distance from the centre (any finite number) | required |
| `(period-ms N)` | milliseconds per cycle, 1-86400000 | required |
| `(phase-deg N)` | starting angle in degrees | `0` |

## Examples

```scheme
(device (unit-id 1)
  (input 1 (type s16) (sine (offset 200) (amplitude 30) (period-ms 86400000))))
```

## See also

- [ramp](ramp.md)
- [noise](noise.md)
