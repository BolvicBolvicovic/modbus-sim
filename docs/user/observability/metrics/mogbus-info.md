# `mogbus_info`

Always 1; the version and the transport of this server.

## Syntax

```text
mogbus_info{version="0.0.1",transport="tcp"}
```

## Valid in

- `gauge`
- always present

## Behaviour

- The `version` label is the mogbus version and `transport` the `(server (mode ...))` of this server.

## Examples

```text
mogbus_info{version="0.0.1",transport="tcp"} 1
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
