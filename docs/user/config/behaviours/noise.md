# `(noise ...)`

Adds a pseudo-random offset to a `static`, `ramp`, `sine` or `lag` value.

## Syntax

```scheme
(noise (amplitude N) [(every-ms N)] [(distribution uniform|gaussian)])
```

## Default

`every-ms 1000`, `distribution uniform`

## Valid in

- `(input ...)`
- `(holding ...)`

## Behaviour

Registers only, and there must be one of the four behaviours in the same point.

| Entry | Meaning | Default |
|---|---|---|
| `(amplitude N)` | `uniform`: offset in `[-N, +N]`; `gaussian`: standard deviation `N`, limited to +-4N. 0 or more | required |
| `(every-ms N)` | a new sample every N milliseconds (held in between), 1-86400000 | `1000` |
| `(distribution ...)` | `uniform` or `gaussian` | `uniform` |

The sample is a fixed function of the device unit id, the point's position in the device and
`floor(time since start / every-ms)`: it is reproducible between runs, ignores `--seed`, does not
depend on how often clients read, and two points get different sequences. For a `lag` the noise is
added to the output and not fed back into the lag. Values go through the point's `type` afterwards:
integer types round to the nearest and clamp to their range.

## Errors

- `(noise ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to disturb`: noise modifies a behaviour; add one.

## Examples

```scheme
(device (unit-id 1)
  (input 2 (type s16)
    (sine (offset 200) (amplitude 30) (period-ms 600000))
    (noise (amplitude 2) (every-ms 500) (distribution gaussian))))
```

## See also

- [degradation](degradation.md)
- [seed](../server/seed.md)
