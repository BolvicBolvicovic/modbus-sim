# `(holding (byte-order ...))`

Swaps the two bytes inside every register of a point.

## Syntax

```scheme
(byte-order big|little)
```

## Default

`big`

## Valid in

- `(holding ...)`
- `(input ...)`

## Behaviour

For every register type (16, 32 and 64-bit, and strings): `big` puts the high byte of every register
first on the wire, as Modbus does. `little` swaps the two bytes inside every register of the point. It
is applied after the [word order](word-order.md), so the two options combine into the four layouts
real 32-bit meters use (bytes A B C D from most to least significant):

| Layout | `word-order` | `byte-order` | Registers (float32 1.5 = `3F C0 00 00`) |
|---|---|---|---|
| ABCD | `high-first` | `big` | `3FC0 0000` |
| CDAB | `low-first` | `big` | `0000 3FC0` |
| BADC | `high-first` | `little` | `C03F 0000` |
| DCBA | `low-first` | `little` | `0000 C03F` |

For 64-bit types the same two options apply to eight bytes A..H: `high-first`/`big` is ABCDEFGH,
`low-first`/`big` is GHEFCDAB (registers reversed), `high-first`/`little` is BADCFEHG (every register
swapped) and `low-first`/`little` is HGFEDCBA (the whole byte string reversed). For 16-bit types only
`byte-order` has an effect (`little` turns `0x1234` into `0x3412`).

## Errors

- `(byte-order ...): expected one of: big, little`: the only byte orders are `big` and `little`.
- `coils and discrete inputs are single bits: remove (byte-order ...)`: byte order is for register points.

## Examples

```scheme
(device (unit-id 1)
  (input 0 (type float32) (static 1.5))                                         ; 3FC0 0000
  (input 2 (type float32) (word-order low-first) (byte-order little) (static 1.5))) ; 0000 C03F
```

Read both back as float32. The second point needs both options undone, `--low-first` for the word order and `--byte-order little` for the bytes:

```bash
./tools/modbus_probe.py input 0 2 --as float32
./tools/modbus_probe.py input 2 2 --as float32 --low-first --byte-order little
```

## See also

- [word-order](word-order.md)
- [importing a register map](../../import/import-overview.md)
