# `mogbus_device_responses_total`

Requests that reached the device, by outcome.

## Syntax

```text
mogbus_device_responses_total{unit="1",result="ok"}
```

## Valid in

- `counter`
- per device
- label `result`

## Behaviour

- The outcomes are the same as in `mogbus_responses_total`: `ok`, `exception`, `none` and `dropped`.

## Examples

```text
mogbus_device_responses_total{unit="1",result="ok"} 1
mogbus_device_responses_total{unit="1",result="exception"} 1
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
