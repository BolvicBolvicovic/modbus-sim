# `mogbus_device_exceptions_total`

Exception responses by exception code.

## Syntax

```text
mogbus_device_exceptions_total{unit="1",code="2"}
```

## Valid in

- `counter`
- per device
- label `code`

## Behaviour

- Counts exceptions from the device and from an `exception` fault alike.
- The `code` label is the exception code in decimal.

## Examples

```text
mogbus_device_exceptions_total{unit="1",code="2"} 1
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
