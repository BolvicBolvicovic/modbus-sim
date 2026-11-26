# `mogbus_control_requests_total`

Runtime control API requests, ok or error.

## Syntax

```text
mogbus_control_requests_total{result="ok"}
```

## Valid in

- `counter`
- label `result`
- needs `(control on)`

## Behaviour

- `error` counts HTTP status 400 and up.
- The series is present only with `(http (control on))`.

## Examples

```text
mogbus_control_requests_total{result="ok"} 10
mogbus_control_requests_total{result="error"} 7
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
