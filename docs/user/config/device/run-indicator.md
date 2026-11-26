# `(device (run-indicator ...))`

Sets the Run Indicator Status byte returned by Report Server ID (FC 17).

## Syntax

```scheme
(run-indicator N)
```

## Default

`255`

## Valid in

- `(device ...)`

## Behaviour

`N` is 0-255 (`0xFF` ON/run, `0x00` OFF by convention).

## See also

- [server-id](server-id.md)
