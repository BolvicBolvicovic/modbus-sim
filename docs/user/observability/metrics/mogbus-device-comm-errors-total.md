# `mogbus_device_comm_errors_total`

Bus communication errors the device counted.

## Syntax

```text
mogbus_device_comm_errors_total{unit="1"}
```

## Valid in

- `counter`
- per device

## Behaviour

- The same count as FC 08 sub-function [0x000C](../../modbus/diagnostics/sub-000c-bus-communication-error-count.md).
- It is a 16-bit counter, so it wraps after 65535.

## Examples

```text
mogbus_device_comm_errors_total{unit="1"} 0
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
