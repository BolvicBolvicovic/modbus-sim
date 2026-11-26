# `mogbus_ready`

1 while the server accepts requests, as `/readyz`.

## Syntax

```text
mogbus_ready
```

## Valid in

- `gauge`
- always present

## Behaviour

- The value is 1 while the server accepts requests and 0 before it starts and while it stops, the same state `/readyz` reports.

## Examples

```text
mogbus_ready 1
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
