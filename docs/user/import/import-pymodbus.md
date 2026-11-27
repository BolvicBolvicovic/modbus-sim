# Importing a pymodbus simulator file

```bash
./tools/mogbus_import.py pymodbus SETUP.json [--device NAME | --all-devices] [--server-name NAME] [--no-server] [--exact-unit]
```

This reads the JSON configuration of the pymodbus 3.x simulator (`server_list` and `device_list`; the
file `pymodbus/server/simulator/setup.json` is the reference). It was checked against pymodbus 3.8.6.

## Options

- `--device NAME` picks a `device_list` entry (default: the first, with a warning if there are more).
- `--all-devices` imports every `device_list` entry into one config, each as its own `(device ...)`
  named after its key, with consecutive unit ids starting at `--unit-id` (or the `device_id` of the
  server, or 1). It excludes `--device`. With more than one device `(any-unit on)` is not written and a
  warning says that only these unit ids answer; with a single device it behaves like the default.
- `--exact-unit` leaves out `(any-unit on)`, so the device answers only its own unit id.
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
| `action` `timestamp` | seven `u16` [`(clock FIELD)`](../config/behaviours/clock.md) points: cell A year, A+1 month, A+2 day, A+3 weekday (Monday 1), A+4 hour, A+5 minute, A+6 second, local time; see below |
| `action` `reset` | the value plus [`(degradation (fail-after-ms 0))`](../config/behaviours/degradation.md): every read covering the point answers exception 04; see below |
| any other `action` (from `custom_actions_dict`) | the value plus an [`(on-read ...)`](../config/actions/on-read.md) that uses a stub [`define-action`](../config/actions/define-action.md); see below |
| `repeat` | the copied cells |
| `identity` of the server | `vendor-name`, `product-code`, `vendor-url`, `product-name`, `model-name`, `major-minor-revision` |
| `server_list` entry | `(server ...)`, see [servers](import-pymodbus-servers.md) |
| `device_id` of the server | the unit id, and no `(any-unit on)` (otherwise `--unit-id`, default 1) |
| a server without `device_id` | `(any-unit on)` on the device: it answers every unit id like pymodbus; `--exact-unit` omits it |
| `device_list` entries | one device per run (`--device`), or all with `--all-devices` |

`increment` and `random` in pymodbus change the value on every read; mogbus values depend on time, so
the importer approximates them with one step per `--action-ms`. An increment whose start value differs
from `minval` starts at `minval`, with a warning.

## Custom Python actions

An action that is not one of the five above is Python code from `custom_actions_dict`, which the
importer cannot translate. For each such action name it writes, once before the first `(device ...)`,
a stub:

```scheme
; TODO: port the pymodbus action "valve_cycle" (Python) to statements
(define-action valve-cycle
  (param period 30)
  (set! self self))
```

and every `uint16`, `uint32`, `float32` or `bits` cell that uses it gets `(on-read (valve-cycle))`.
The point keeps its value, so the config loads and `--check` passes, but reading it changes nothing
until you replace the body with [statements](../config/machines/statements.md) (`self` is the point).

- The name is the action name lowercased, with other characters turned into `-`; names that would
  not be valid (a leading digit, a statement or expression keyword, more than 63 characters) are
  adjusted and repeated names get a number. The original name stays in the comment.
- A `parameters` entry whose value is a number or a boolean becomes a `(param ...)` with the value of
  the first cell using the action as default; other entries are listed in a comment and skipped. A
  cell whose value differs gives an override such as `(on-read (valve-cycle (period 60)))`.
- A cell never shares a point with its neighbours (no `(count N)`), because `on-read` needs a single
  point. A `bits` cell becomes 16 coil and 16 discrete points that each carry the action.
- A warning names every imported action, so it is not forgotten; `--strict` makes it an error.
- Strings do not accept `on-read`: a custom action on a string keeps the static value and warns.

## The timestamp action

pymodbus declares it as `"action": "timestamp"` on a `uint16` entry (it also works on the first cell of
a `uint32` or `float32`). When a client reads that cell, pymodbus overwrites the seven cells from
there on with year, month, weekday and so on, so the six following cells have to exist in the file
(declared as plain `uint16` cells, usually in the same range) and are rewritten whatever they hold.
The importer makes the seven cells at A..A+6 clock points and warns about every declared cell it
replaces and about cells that are not declared. A range such as `[A, A+6]` with the action puts the
action on every cell; the importer keeps one block starting at A and warns about the others.

Two differences remain. pymodbus writes the month as 0-11 (`month - 1`), a `(clock month)` point
shows 1-12; the importer warns, and the client has to subtract 1 (or ignore it). pymodbus refreshes
the seven cells only when the first cell is read; mogbus clock points are correct whichever cell is
read.

## The reset action

In pymodbus, reading a cell with `"action": "reset"` raises inside the datastore (actions run on reads
only, never on writes), and the server answers exception 04 (slave failure); nothing restarts and the
value is kept. The importer maps the cell to its value plus `(degradation (fail-after-ms 0))`: every
read covering the point answers exception 04, writes do not change that, and it fails again right after
a device restart. On a `bits` cell the same is applied to the 16 coil and discrete points of the cell,
because pymodbus also runs the action on bit reads.

One difference remains: pymodbus puts function code 0x00 in that reply (a pymodbus quirk, so the
function byte is 0x80), while mogbus answers with the request's function code. Clients normally check
only the exception code, so the importer does not warn about it.

## Several devices

`--all-devices` writes every `device_list` entry as a device of the same config. pymodbus answers
every unit id with the first device; mogbus routes by exact unit id, so a client has to use the ids
the importer assigned (`unit-id` lines and the warning). Without the option only one device is
imported.

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
