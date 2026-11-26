# Importing a register map

If you already have a register map, do not retype it. `tools/mogbus_import.py` turns it into a
mogbus config. It needs Python 3.9 or newer and nothing else (no pip packages, no mogbus library).

```bash
./tools/mogbus_import.py csv examples/import/meter-map.csv --server tcp --port 1502 -o meter.scm
./tools/mogbus_import.py pymodbus examples/import/pymodbus-setup.json -o plant.scm
./mogbus --check meter.scm
```

The config goes to stdout, or to the file given with `-o`. It starts with a comment saying where it
came from. Every point keeps its name, description and unit as a `(note "...")`.

Things the importer cannot map, or is unsure about, are **warnings**. They are printed to stderr and
written into the output as `;` comments, so they stay with the file. Read them: they tell you
where the config differs from the original. Errors (for example two points on the same address)
stop the import and write nothing.

| Option (both formats) | Meaning |
|---|---|
| `-o FILE` | write the config to `FILE` instead of stdout |
| `--strict` | treat every warning as an error: exit status 1, nothing written |
| `--check` | run `$MOGBUS_BIN` (default `./mogbus`) `--check` on the result; a failing check exits 1 |
| `--unit-id N` | Modbus unit id of the device (default 1) |
| `--name TEXT` | device name (default: the file name, or the pymodbus device name) |

Exit status: 0 success, 1 errors, warnings under `--strict` or a failed `--check`, 2 bad arguments.

## CSV register maps

```bash
./tools/mogbus_import.py csv MAP.csv [options]
```

The importer detects the delimiter (`,` `;` or tab) and the header row; title lines above the header
are skipped. Columns are found by their header, ignoring case, spaces, `_`, `-` and a trailing
`(...)` such as `Address (dec)`. Only the address column is required.

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

If your headers are different, name them: `--map address=Reg,type=Kind,name="Signal name"` (`field=Header`,
comma separated; fields are the names in the left column). `--map` wins over the aliases.

Rows without an address (section titles) are skipped; a row whose address or data type cannot be read
is skipped with a warning. Lines starting with `#` or `;` in the first cell are comments.

### Tables

The `space` cell is read as: `holding`, `input`, `coil`, `discrete` (also "holding register", "input
register", "discrete input", "coil status", "input status", ...), the Modicon prefixes `4x` `3x` `0x` `1x`,
or a function code (`3`, `FC04`, `0x03`): 1, 5, 15 are coils, 2 discrete inputs, 3, 6, 16, 22, 23 holding
registers, 4 input registers.

Without a `space` cell the table comes from the address (Modicon style, below), then from the data
type (a `bool` is a discrete input when the access column says read-only, else a coil), then from
`--space` (default `holding`). A `bool` in a register table is stored as a `u16` with a warning.

### Address base

mogbus addresses are the 0-based numbers sent on the wire. Manuals often count from 1.

- **Modicon addresses.** If every address is a 5 or 6 digit number whose first digit is 0, 1, 3
  or 4 (`40001`, `300017`, `00005`), they are Modicon numbers: the first digit selects the table
  (0 coil, 1 discrete input, 3 input register, 4 holding register) and the rest counts from 1, so
  `40001` is `holding 0` and `30003` is `input 2`. If only some addresses look like that, none is
  treated as Modicon and a warning says so. `--modicon yes|no|auto` forces the choice.
- **Other addresses.** `--base 0|1|auto` says what the first address is. With `auto` the base is 1
  when the smallest address in the file is 1 and no address is 0 (with a warning), otherwise 0.
  `--base 1` subtracts 1 from every address. For Modicon numbers `--base` applies after the first
  digit is removed (`auto` is 1).
- Hex addresses (`0x10`, `10h`, `$10`) are read as hex. A bit address such as `40001.3` is not
  supported: the row is skipped with a warning.

A table cell, when present, wins over the Modicon prefix (with a warning if they disagree).

### Data types

| mogbus | Accepted names |
|---|---|
| `u16` | u16, uint16, word, unsigned, uint, unsigned short, "unsigned 16-bit" |
| `s16` | s16, int16, short, signed, int, integer, sint |
| `u32` | u32, uint32, udint, dword, unsigned long, ulong |
| `s32` | s32, int32, dint, long |
| `float32` | float, float32, real, ieee754, single, f32, "32-bit float" |
| `u64` / `s64` | u64, uint64, ulint, lword / s64, int64, lint |
| `float64` | double, float64, lreal, f64 |
| bool | bool, bit, boolean, flag (coils and discrete inputs) |
| `string` | string, ascii, char, text |

With no type column (or an empty cell) the type is `--type` (default `u16`). An unknown type skips
the row with a warning. BCD and other encodings are not supported.

**String length.** A string occupies a whole number of registers (2 characters each, at most 123).
The importer decides in this order:

1. A number in the type cell, `string(10)`, `STRING[20]`, `ascii 16`, counts **characters**
   (bytes): `string(10)` is 5 registers, `string(7)` is 4. Add `regs` or `words` to count
   registers: `string(10 regs)`.
2. Otherwise the length column, in **registers**, unless its header or cell says bytes or
   characters (`Length (bytes)`, `12 chars`).
3. Otherwise it is an error.

A string default that does not fit is truncated with a warning; characters outside printable ASCII
become `?`.

### Byte layout

mogbus describes the four common 32-bit layouts with `word-order` and `byte-order` (see
[the config reference](config-reference.md#byte-order-)). The layout cell (or the type cell, as in
`float32 CDAB`) is read as:

| Layout | Written as | mogbus |
|---|---|---|
| ABCD | `ABCD`, `1234`, `A B C D`, big endian, BE | defaults |
| CDAB | `CDAB`, `3412`, word swap(ped), mid-little endian | `(word-order low-first)` |
| BADC | `BADC`, `2143`, byte swap(ped), mid-big endian | `(byte-order little)` |
| DCBA | `DCBA`, `4321`, little endian, LE, "word and byte swap" | `(word-order low-first) (byte-order little)` |

In separate `word order` / `byte order` columns, `big`, `high first`, `msw` and `no` mean the default,
`little`, `low first`, `lsw`, `swap` and `yes` mean swapped. A bare `swap` column with `yes` is
ambiguous and is taken as a word swap with a warning. `--layout ABCD|CDAB|BADC|DCBA` is the layout
for rows that say nothing (default `ABCD`). For 16-bit types only the byte order matters; strings
take the byte order only.

### Values, access and scale

- The `default` cell becomes `(static ...)`: numbers as numbers, `on/off/true/false/1/0` for bits,
  text for strings. Out-of-range values are clamped with a warning. Without a default the point is 0.
- `access` (R, RO, read only, W, RW, R/W, ...) is only checked. A read-only entry in a writable
  table (`holding`, `coil`) or a writable one in `input`/`discrete` gives a warning; the table is
  never changed, because the table is what the map says.
- A `scale` other than 1 gives a warning and is added to the note (`raw x 0.1`). mogbus stores raw
  register values and does not scale, so put the raw value in the default: for 230.4 V with scale
  0.1, the default is `2304`.

### Overlaps and gaps

A multi-register point occupies several addresses (`u32` two, `u64` four, a string its length). Two
points that share an address in one table are an **error**. The same address in different tables is
fine. A gap between points is fine: addresses without a point answer exception 02, like a real
device, and a read that spans a gap fails the same way.

### Device and server

`--server MODE` writes a `(server ...)` section (`tcp`, `udp`, `rtu-tcp`, `rtu-udp`, `ascii-tcp`,
`ascii-udp`, `rtu-pty`, `ascii-pty`, `rtu-device`, `ascii-device`), with `--port`, `--bind`, `--path`
(device modes) and `--baud`. Without `--server` there is no server section and mogbus uses its
defaults.

### Example

[`examples/import/meter-map.csv`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/meter-map.csv) is a made-up meter map with
Modicon addresses, CDAB floats, a `u64` counter, strings, units, a scale and an access column. The
committed result, [`meter-map.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/meter-map.scm), is made with:

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

## pymodbus simulator files

```bash
./tools/mogbus_import.py pymodbus SETUP.json [--device NAME] [--server-name NAME] [--no-server]
```

This reads the JSON configuration of the pymodbus 3.x simulator (`server_list` and `device_list`; the
file `pymodbus/server/simulator/setup.json` is the reference). It was checked against pymodbus
3.8.6. Options: `--device NAME` picks a `device_list` entry (default: the first, with a warning if
there are more), `--server-name NAME` picks the `server_list` entry (default: the first, warning
likewise), `--no-server` omits the `(server ...)` section, `--action-ms N` sets the step time of
increments and random values (default 1000).

### What is mapped

| pymodbus | mogbus |
|---|---|
| `uint16` entry (`addr` N or `[first, last]`) | `u16` points; equal neighbours are merged with `(count N)` |
| `uint32` | `u32`, high word first (pymodbus layout); a single `addr` N occupies N and N+1 |
| `float32` | `float32` |
| `string` `[first, last]` | `string`, `(length last-first+1)`, `(padding space)` |
| `bits` | one cell is 16 bits: cell `c` holds coils `16c` to `16c+15` (bit 0 first); `value` is the bit mask |
| `value`, `defaults.value` | `(static ...)`; entry value wins over the default |
| `action` `increment` | `(ramp (from min) (to max) (step 1) (every-ms N) (mode wrap))`; min and max from `parameters` (`minval`, `maxval`), else the start value and the type maximum (`float32`: 16777216) |
| `action` `random` | `(static middle)` plus `(noise (amplitude half-range) (every-ms N))` over `minval`..`maxval` (default 1..65536) |
| `action` `uptime` | a ramp from 1 that counts seconds and holds at the type maximum |
| `repeat` | the copied cells |
| `identity` of the server | `vendor-name`, `product-code`, `vendor-url`, `product-name`, `model-name`, `major-minor-revision` |
| `server_list` entry | `(server ...)`, see below |
| `device_id` of the server | the unit id (otherwise `--unit-id`, default 1) |

`increment` and `random` in pymodbus change the value on every read; mogbus values depend on time,
so the importer approximates them with one step per `--action-ms`. An increment whose start value
differs from `minval` starts at `minval`, with a warning.

**Shared blocks.** pymodbus with `"shared blocks": true` serves one array of cells for all tables.
mogbus has four separate tables, so every register is emitted twice, in `holding` and `input`
with the same address, and every bit cell twice, in `coil` and `discrete`, as the notes in the
output say. With `"shared blocks": false` the blocks are laid out one after the other (coils, discrete
inputs, input registers, holding registers, sizes from the `... size` entries) and every cell goes to
the table of its block; cells that no function code can reach are skipped with a warning.

**Write access.** pymodbus makes everything read-only except the cells in `write`. mogbus holding
registers and coils are always writable, input registers and discrete inputs never. So `write` cannot
be reproduced exactly. The importer keeps all registers in `holding` (so reads with function code
03 work) and `input`, and warns once how many registers or bits were not in `write`. If a point must
not be writable, delete its `holding` line by hand; the `input` copy stays.

**Servers.**

| `comm` / `framer` | mogbus mode |
|---|---|
| `tcp` / `socket` | `tcp` (`host` becomes `bind`, `port` the port) |
| `tcp` / `rtu` | `rtu-tcp` |
| `tcp` / `ascii` | `ascii-tcp` |
| `udp` / `socket`, `rtu`, `ascii` | `udp`, `rtu-udp`, `ascii-udp` |
| `serial` / `rtu`, `ascii` | `rtu-device`, `ascii-device` (`port` is the path, `baudrate`, `parity` N/E/O, `stopbits`, and for ASCII `bytesize` 7 or 8) |
| `tls` / `tls` | `tcp` with `(tls (certificate ...) (private-key ...) (require-client-certificate off))` and a warning: add a `ca-certificate` to require client certificates |

Anything else gets a warning and no server section.

### Not supported

- The actions `timestamp` and `reset`, and custom actions: the initial value is kept as a static value
  and a warning is written. Actions on `bits` and strings likewise.
- `invalid`: ignored. Every address without a point already answers exception 02 in mogbus.
- `type exception` (rejecting requests that split a 32-bit value) and the pymodbus HTTP interface.
- A pymodbus server answers every unit id; mogbus answers the unit id of the device only.
- Several devices in one file: one device is imported per run (`--device NAME`); to serve several
  devices, run the importer once for each and merge the `(device ...)` forms by hand.
- Bit addresses above coil 65535.

### Example

[`examples/import/pymodbus-setup.json`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/pymodbus-setup.json) has two devices and
two servers; the committed result [`pymodbus-setup.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/pymodbus-setup.scm) is made
with:

```bash
./tools/mogbus_import.py pymodbus examples/import/pymodbus-setup.json --device plant \
    --server-name main -o examples/import/pymodbus-setup.scm
```
