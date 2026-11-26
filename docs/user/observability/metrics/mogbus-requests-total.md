# `mogbus_requests_total`

Decoded requests by function code.

## Syntax

```text
mogbus_requests_total{fc="3"}
```

## Valid in

- `counter`
- label `fc`

## Behaviour

- Counts requests to unknown units and broadcasts too.
- The `fc` label is the function code in decimal.
- `mogbus --health` is a client too: each probe counts as an FC 08 request (`fc="8"`) to the first device.

## Examples

```text
mogbus_requests_total{fc="3"} 2
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
