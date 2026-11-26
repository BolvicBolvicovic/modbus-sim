# `mogbus_connections_active`

Open client connections.

## Syntax

```text
mogbus_connections_active
```

## Valid in

- `gauge`
- TCP modes

## Behaviour

- TCP modes only. The UDP and serial modes have no connections, so it stays at 0 there.

## Examples

```text
mogbus_connections_active 0
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
