# `mogbus_start_time_seconds`

Start time of the server, in Unix seconds.

## Syntax

```text
mogbus_start_time_seconds
```

## Valid in

- `gauge`
- always present

## Behaviour

- Use it to compute the uptime or to detect restarts.

## Examples

```text
mogbus_start_time_seconds 1795730501
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
