# `(device (com-status ...))`

Sets the status field returned by Get Com Event Counter (FC 11) and Get Com Event Log (FC 12).

## Syntax

```scheme
(com-status N)
```

## Default

`0`

## Valid in

- `(device ...)`

## Behaviour

`0` (ready) or `65535` (busy). Any other value is rejected by `--check` and at startup.

## Examples

```scheme
(device (unit-id 1) (com-status 65535) (holding 0 (static 1)))
```

## See also

- [com-event](com-event.md)
