# `(device (server-id-additional-data ...))`

Sets the Additional Data returned by Report Server ID (FC 17).

## Syntax

```scheme
(server-id-additional-data "HEXBYTES")
```

## Default

`""` (none)

## Valid in

- `(device ...)`

## Behaviour

An even-length hex string of 0-249 bytes; the empty string is allowed.

## Examples

```scheme
(device (unit-id 1) (server-id-additional-data "0102AA") (holding 0 (static 1)))
```

## See also

- [server-id](server-id.md)
