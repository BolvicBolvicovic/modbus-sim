# `mogbus_device_requests_total`

Requests that reached the device.

## Syntax

```text
mogbus_device_requests_total{unit="1"}
```

## Valid in

- `counter`
- per device

## Behaviour

- Requests for unknown units are not counted here; see `mogbus_requests_total`.

## Examples

```text
mogbus_device_requests_total{unit="1"} 2
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
