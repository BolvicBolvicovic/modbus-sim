# `(device (exception-status ...))`

Sets the byte returned by Read Exception Status (FC 07).

## Syntax

```scheme
(exception-status N)
```

## Default

`0`

## Valid in

- `(device ...)`

## Behaviour

`N` is 0-255.

## Examples

```scheme
(device (unit-id 1) (exception-status 5) (holding 0 (static 1)))
```

## See also

- [Modbus support](../../modbus/modbus-overview.md)
