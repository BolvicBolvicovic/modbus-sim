# `(device (server-id ...))`

Sets the Server ID byte returned by Report Server ID (FC 17).

## Syntax

```scheme
(server-id N)
```

## Default

`0`

## Valid in

- `(device ...)`

## Behaviour

`N` is 0-255.

## Examples

```scheme
(device (unit-id 1) (server-id 42) (run-indicator 255) (holding 0 (static 1)))
```

## See also

- [run-indicator](run-indicator.md)
- [server-id-additional-data](server-id-additional-data.md)
