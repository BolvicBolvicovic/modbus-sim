# `mogbus_connections_total`

Accepted client connections.

## Syntax

```text
mogbus_connections_total
```

## Valid in

- `counter`
- TCP modes

## Behaviour

- TCP modes only. The UDP and serial modes have no connections, so it stays at 0 there.

## Examples

```text
mogbus_connections_total 1
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
