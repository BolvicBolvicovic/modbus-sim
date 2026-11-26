# `mogbus_device_faults_total`

Injected faults that fired, by kind.

## Syntax

```text
mogbus_device_faults_total{unit="1",kind="delay"}
```

## Valid in

- `counter`
- per device
- label `kind`

## Behaviour

- Kinds are `delay`, `drop`, `exception`, `corrupt`, `disconnect`, `restart` and `sensor-disconnect`.
- Configured kinds appear even at 0.

## Examples

```text
mogbus_device_faults_total{unit="1",kind="delay"} 4
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
