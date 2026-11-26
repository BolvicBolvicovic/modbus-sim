# `mogbus_responses_total`

Requests by outcome: ok, exception, none or dropped.

## Syntax

```text
mogbus_responses_total{result="ok"}
```

## Valid in

- `counter`
- label `result`

## Behaviour

- `ok`: a normal response was sent. `exception`: an exception response.
- `none`: no response by protocol (broadcast, unknown RTU or ASCII unit).
- `dropped`: no response because of a drop or disconnect fault.

## Examples

```text
mogbus_responses_total{result="ok"} 1
mogbus_responses_total{result="exception"} 1
mogbus_responses_total{result="none"} 0
mogbus_responses_total{result="dropped"} 0
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
