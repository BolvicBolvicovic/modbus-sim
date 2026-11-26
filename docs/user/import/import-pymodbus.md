# Importing a pymodbus simulator file

```bash
./tools/mogbus_import.py pymodbus SETUP.json [--device NAME] [--server-name NAME] [--no-server]
```

This reads the JSON configuration of the pymodbus 3.x simulator (`server_list` and `device_list`; the
file `pymodbus/server/simulator/setup.json` is the reference). It was checked against pymodbus 3.8.6.

## Options

- `--device NAME` picks a `device_list` entry (default: the first, with a warning if there are more).
- `--server-name NAME` picks the `server_list` entry (default: the first, warning likewise).
- `--no-server` omits the `(server ...)` section.
- `--action-ms N` sets the step time of increments and random values (default 1000).

## What is mapped

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
| `server_list` entry | `(server ...)`, see [servers](import-pymodbus-servers.md) |
| `device_id` of the server | the unit id (otherwise `--unit-id`, default 1) |

`increment` and `random` in pymodbus change the value on every read; mogbus values depend on time, so
the importer approximates them with one step per `--action-ms`. An increment whose start value differs
from `minval` starts at `minval`, with a warning.

## Shared blocks

pymodbus with `"shared blocks": true` serves one array of cells for all tables. mogbus has four
separate tables, so every register is emitted twice, in `holding` and `input` with the same address,
and every bit cell twice, in `coil` and `discrete`, as the notes in the output say. With
`"shared blocks": false` the blocks are laid out one after the other (coils, discrete inputs, input
registers, holding registers, sizes from the `... size` entries) and every cell goes to the table of
its block; cells that no function code can reach are skipped with a warning.

## Write access

pymodbus makes everything read-only except the cells in `write`. The importer keeps every cell in
`holding` (so reads with function code 03 work) and `input`, and marks the `holding` and `coil` copies
of cells that are not in `write` as `(access read-only)`; the `input` and `discrete` copies are
read-only anyway.

## Example

[`examples/import/pymodbus-setup.json`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/pymodbus-setup.json)
has two devices and two servers; the committed result
[`pymodbus-setup.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/import/pymodbus-setup.scm)
is made with:

```bash
./tools/mogbus_import.py pymodbus examples/import/pymodbus-setup.json --device plant \
    --server-name main -o examples/import/pymodbus-setup.scm
```

## See also

- [Servers and what is not supported](import-pymodbus-servers.md)
- [Importing a register map](import-overview.md)
