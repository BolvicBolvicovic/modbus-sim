# Importing a CSV register map

```bash
./tools/mogbus_import.py csv MAP.csv [options]
```

The importer detects the delimiter (`,` `;` or tab) and the header row; title lines above the header
are skipped. Columns are found by their header, ignoring case, spaces, `_`, `-` and a trailing `(...)`
such as `Address (dec)`. Only the address column is required.

## Columns

| Field | Header names it recognizes |
|---|---|
| `address` | address, register, reg, offset, addr, modbus address, register address, reg address, register number, reg no |
| `space` | table, space, register type, type of register, function code, fc, object type, reg type, modbus table, function |
| `type` | type, data type, format, datatype, value type, data format |
| `layout` | layout, endianness, endian, swap, byte layout, byte sequence, register order |
| `word_order` | word order, word swap |
| `byte_order` | byte order, byte swap |
| `name` | name, parameter, signal, tag, point, register name, parameter name, variable, label |
| `description` | description, desc, comment, comments, remarks, notes, details, meaning |
| `unit` | unit, units, uom, engineering unit, eu |
| `default` | default, value, initial, initial value, default value, init, preset |
| `access` | access, rw, r/w, read/write, permission, permissions, read write |
| `scale` | scale, multiplier, factor, gain, scaling, scale factor, coefficient |
| `length` | length, size, len, registers, count, number of registers, no of registers, words, qty, quantity, regs, reg count |

If your headers are different, name them: `--map address=Reg,type=Kind,name="Signal name"`
(`field=Header`, comma separated; fields are the names in the left column). `--map` wins over the
aliases.

Rows without an address (section titles) are skipped; a row whose address or data type cannot be read
is skipped with a warning. Lines starting with `#` or `;` in the first cell are comments.

## Tables

The `space` cell is read as: `holding`, `input`, `coil`, `discrete` (also "holding register", "input
register", "discrete input", "coil status", "input status", ...), the Modicon prefixes `4x` `3x` `0x`
`1x`, or a function code (`3`, `FC04`, `0x03`): 1, 5, 15 are coils, 2 discrete inputs, 3, 6, 16, 22,
23 holding registers, 4 input registers.

Without a `space` cell the table comes from the address (Modicon style, see
[addresses](import-csv-addresses.md)), then from the data type (a `bool` is a discrete input when the
access column says read-only, else a coil), then from `--space` (default `holding`). A `bool` in a
register table is stored as a `u16` with a warning.

## Overlaps and gaps

A multi-register point occupies several addresses (`u32` two, `u64` four, a string its length). Two
points that share an address in one table are an **error**. The same address in different tables is
fine. A gap between points is fine: addresses without a point answer exception 02, like a real
device, and a read that spans a gap fails the same way.

## Device and server

`--server MODE` writes a `(server ...)` section (`tcp`, `udp`, `rtu-tcp`, `rtu-udp`, `ascii-tcp`,
`ascii-udp`, `rtu-pty`, `ascii-pty`, `rtu-device`, `ascii-device`), with `--port`, `--bind`, `--path`
(device modes) and `--baud`. Without `--server` there is no server section and mogbus uses its
defaults.

## Example

[`examples/import/meter-map.csv`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/meter-map.csv)
is a made-up meter map with Modicon addresses, CDAB floats, a `u64` counter, strings, units, a scale
and an access column. The committed result,
[`meter-map.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/meter-map.scm), is
made with:

```bash
./tools/mogbus_import.py csv examples/import/meter-map.csv --server tcp --port 1502 \
    --name "Acme PM-300" -o examples/import/meter-map.scm
```

```text
  (input 0 (type float32) (word-order low-first) (static 230.4)
      (note "Voltage L1 - Phase to neutral voltage (V)"))
  (input 10 (static 5000) (note "Frequency - Line frequency (Hz), raw x 0.01"))
  (input 12 (type u64) (static 1234567890) (note "Active energy - Total imported energy (Wh)"))
```

A test regenerates the committed files and fails if they differ, so they always match the tool.

## See also

- [Address base and Modicon addresses](import-csv-addresses.md)
- [Data types, byte layout and values](import-csv-types.md)
- [Importing a register map](import-overview.md)
