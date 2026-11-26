# `(ramp ...)`

A register value that moves between two numbers over time.

## Syntax

```scheme
(ramp (from N) (to N) (step N) (every-ms N) (mode wrap|bounce|hold))
```

## Default

`mode wrap`

## Valid in

- `(holding ...)`
- `(input ...)`

## Behaviour

The value advances by `step` every `every-ms` milliseconds. It is computed when a client reads it, so
there is no background thread.

| Mode     | At the end                 |
|----------|----------------------------|
| `wrap`   | jumps back to `from`       |
| `bounce` | walks back                 |
| `hold`   | stays on the last value    |

## Errors

`step` must be positive, `every-ms` must be at least 1.

## Examples

```scheme
(holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap)))
```

```python
dev.set_value(HOLDING, 1, 0)
```

## See also

- [server mode](server-mode.md)
