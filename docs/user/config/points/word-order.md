# `(holding (word-order ...))`

Chooses which register holds the most significant part of a 32- or 64-bit value.

## Syntax

```scheme
(word-order high-first|low-first)
```

## Default

`high-first`

## Valid in

- `(holding ...)`
- `(input ...)`

## Behaviour

For 32- and 64-bit types only.

- `high-first` (default): the first register is the most significant. This is the usual "big-endian" layout.
- `low-first`: the registers come in reverse order, the first one is the least significant. Some
  devices do this ("word swapped").

Combined with [`byte-order`](byte-order.md) it gives the four layouts real meters use.

## Errors

- `strings have no word order: remove (word-order ...)`: only `byte-order` applies to strings.

## Examples

```scheme
(device (unit-id 1)
  (input 0 (type u32) (static 65536))                        ; 0001 0000
  (input 2 (type u32) (word-order low-first) (static 65536))) ; 0000 0001
```

## See also

- [byte-order](byte-order.md)
- [type](type.md)
