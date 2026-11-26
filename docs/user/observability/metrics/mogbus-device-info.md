# `mogbus_device_info`

Always 1, one series per device.

## Syntax

```text
mogbus_device_info{unit="1",name="Meter"}
```

## Valid in

- `gauge`
- per device

## Behaviour

- The labels carry the unit id and the device name.

## Examples

```text
mogbus_device_info{unit="1",name="Meter"} 1
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
