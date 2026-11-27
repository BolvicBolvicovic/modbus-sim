# Data types, byte layout and values

How the importer reads the type, layout, default, access and scale columns of a CSV register map.

## Data types

| mogbus | Accepted names |
|---|---|
| `u16` | u16, uint16, word, unsigned, uint, unsigned short, "unsigned 16-bit" |
| `s16` | s16, int16, short, signed, int, integer, sint |
| `u32` | u32, uint32, udint, dword, unsigned long, ulong |
| `s32` | s32, int32, dint, long |
| `float32` | float, float32, real, ieee754, single, f32, "32-bit float" |
| `u64` / `s64` | u64, uint64, ulint, lword / s64, int64, lint |
| `float64` | double, float64, lreal, f64 |
| `bool` | bool, bit, boolean, flag (coils and discrete inputs) |
| `string` | string, ascii, char, text |

With no type column (or an empty cell) the type is `--type` (default `u16`). An unknown type skips the
row with a warning. BCD and other encodings are not supported.

### String length

A string occupies a whole number of registers (2 characters each, at most 123). The importer decides
in this order:

1. A number in the type cell, `string(10)`, `STRING[20]`, `ascii 16`, counts **characters** (bytes):
   `string(10)` is 5 registers, `string(7)` is 4. Add `regs` or `words` to count registers:
   `string(10 regs)`.
2. Otherwise the length column, in **registers**, unless its header or cell says bytes or characters
   (`Length (bytes)`, `12 chars`).
3. Otherwise it is an error.

A string default that does not fit is truncated with a warning; characters outside printable ASCII
become `?`.

## Byte layout

mogbus describes the four common 32-bit layouts with `word-order` and `byte-order` (see the
[config reference](../config/points/byte-order.md)). The layout cell (or the type cell, as in
`float32 CDAB`) is read as:

| Layout | Written as | mogbus |
|---|---|---|
| ABCD | `ABCD`, `1234`, `A B C D`, big endian, BE | defaults |
| CDAB | `CDAB`, `3412`, word swap(ped), mid-little endian | `(word-order low-first)` |
| BADC | `BADC`, `2143`, byte swap(ped), mid-big endian | `(byte-order little)` |
| DCBA | `DCBA`, `4321`, little endian, LE, "word and byte swap" | `(word-order low-first) (byte-order little)` |

In separate `word order` and `byte order` columns, `big`, `high first`, `msw` and `no` mean the
default; `little`, `low first`, `lsw`, `swap` and `yes` mean swapped. A bare `swap` column with `yes`
is ambiguous and is taken as a word swap with a warning. `--layout ABCD|CDAB|BADC|DCBA` is the layout
for rows that say nothing (default `ABCD`). For 16-bit types only the byte order matters; strings take
the byte order only.

## Values, access and scale

- The `default` cell becomes `(static ...)`: numbers as numbers, `on/off/true/false/1/0` for bits, text
  for strings. Out-of-range values are clamped with a warning. Without a default the point is 0.
- `access` (R, RO, read only, W, RW, R/W, ...): a read-only entry in `holding` or `coil` becomes
  `(access read-only)`, so clients cannot write it. A writable entry in `input` or `discrete` gives a
  warning; the table is never changed, because the table is what the map says.
- A `scale` other than 1 gives a warning and is added to the note (`raw x 0.1`). mogbus stores raw
  register values and does not scale, so put the raw value in the default: for 230.4 V with scale 0.1,
  the default is `2304`.

## See also

- [Importing a CSV register map](import-csv.md)
- [Address base and Modicon addresses](import-csv-addresses.md)
