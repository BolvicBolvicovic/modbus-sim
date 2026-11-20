# Config reference

A config file describes the simulator: which port to use and which devices exist.
Save it with any name, for example `my-device.scm`, and run `./modbus-sim my-device.scm`.

## Scheme in one minute

The file format comes from Scheme, but you only need these rules:

- Everything is in parentheses: `(name value)`. Every `(` needs a matching `)`.
- `;` starts a comment that runs to the end of the line.
- Text is in double quotes: `"Power meter"`.
- Numbers are written normally: `42`, `-5`, `0.5`, `230.4`.
- On/off can be written `on`, `off`, `yes`, `no`, `true`, `false`, `#t`, `#f`, `1` or `0`.
- Spaces, tabs and new lines are all the same. Indent however you like.
- Order of entries does not matter, except that a point can only be defined once.

The file is only *read as data*. Nothing in it is run, so a typo can never do anything but
produce an error message.

## Overall shape

```scheme
(server ...)    ; optional, at most once

(device ...)    ; one or more
(device ...)
```

## `(server ...)`

| Entry | Meaning | Default |
|---|---|---|
| `(port N)` | TCP port, 1-65535 | `1502` |
| `(bind "ADDRESS")` | IPv4 address to listen on. `"0.0.0.0"` = all interfaces, `"127.0.0.1"` = this machine only | `"0.0.0.0"` |
| `(seed N)` | Seed for the random dice used by faults, 0-4294967295. Same seed gives the same sequence of faults | `0` (a fixed built-in seed) |

The command-line options `--port` and `--bind` override these.

## `(device ...)`

| Entry | Meaning | Default |
|---|---|---|
| `(name "Text")` | Name shown in logs and `--check`, up to 63 characters | `"unnamed"` |
| `(unit-id N)` | Modbus unit id (slave address), 0-255 | `1` |
| `(exception-status N)` | Byte returned by Read Exception Status (FC 07), 0-255 | `0` |
| `(server-id N)` | Server ID byte returned by Report Server ID (FC 17), 0-255 | `0` |
| `(run-indicator N)` | Run Indicator Status byte returned by Report Server ID (FC 17), 0-255 (`0xFF` ON/run, `0x00` OFF by convention) | `255` |
| `(note "Text")` | A comment kept for humans. Can be repeated. Ignored by the program | - |
| `(fault ...)` | One fault, see below | none |
| `(file ...)` | File record data for Read File Record (FC 20), see below | none |
| `(coil ...)` `(discrete ...)` `(input ...)` `(holding ...)` | Data points, see below | - |

Unit ids must be different between devices.

## Data points

There are four Modbus tables. Use the one your real device uses.

| Entry | Table | Holds | Can a client write it? | Function codes |
|---|---|---|---|---|
| `coil` | Coils | on/off | yes | 01, 05, 15 |
| `discrete` | Discrete inputs | on/off | no | 02 |
| `input` | Input registers | numbers | no | 04 |
| `holding` | Holding registers | numbers | yes | 03, 06, 16 |

Shape of a point:

```scheme
(holding ADDRESS
  (type ...)          ; registers only, default u16
  (word-order ...)    ; 32-bit types only, default high-first
  (static VALUE)      ; or (ramp ...), not both. Without either, the value is 0 / off
  (count N)           ; optional: repeat for N consecutive points
  (note "Text"))      ; optional comment
```

`ADDRESS` is the Modbus address as the client sends it on the wire (0-65535). If your device's
manual says "40001", that is address `0` of the holding table; "40101" is `100`.

**Addresses you do not define do not exist.** A client reading or writing one gets exception 02
(illegal data address), like a real device. A read that covers a gap also fails.

### `(type ...)`

| Type | Registers used | Range |
|---|---|---|
| `u16` | 1 | 0 to 65535 |
| `s16` | 1 | -32768 to 32767 |
| `u32` | 2 | 0 to 4294967295 |
| `s32` | 2 | -2147483648 to 2147483647 |
| `float32` | 2 | IEEE 754 single precision |

Coils and discrete inputs are always on/off; do not give them a `type`.
Values are rounded to the nearest whole number and limited to the range of the type.

A 32-bit point occupies two addresses: `(input 4 (type u32) ...)` uses addresses 4 and 5.

### `(word-order ...)`

For 32-bit types: which of the two registers holds the high half.

- `high-first` (default): the first register is the high word. This is the usual "big-endian" layout.
- `low-first`: the first register is the low word. Some devices do this ("word swapped").

### `(static VALUE)`

The value never changes by itself.

```scheme
(holding 0 (static 230))
(input 2 (type float32) (static 0.97))
(coil 0 (static on))
```

### `(ramp ...)`

The value moves from `from` towards `to`, by `step`, once every `every-ms` milliseconds.

| Entry | Meaning | Default |
|---|---|---|
| `(from N)` | first value | required |
| `(to N)` | last value | required |
| `(step N)` | how much it changes each time. Always positive; the direction follows from `from` and `to` | `1` |
| `(every-ms N)` | milliseconds between steps, 1-86400000 | `1000` |
| `(mode ...)` | what happens at the end, see below | `wrap` |

Modes:

- `wrap`: jumps back to `from` and starts again (a sawtooth). `0 10 20 30 0 10 ...`
- `bounce`: turns around and walks back (a triangle). `0 10 20 30 20 10 0 10 ...`
- `hold`: stays on the last value. `0 10 20 30 30 30 ...`

The value never goes beyond `to`. If the step does not divide the range evenly, the last value
is the last one that fits: from 0 to 25 by 10 gives `0 10 20`.

Ramps start when the simulator starts, and every ramp in the file starts at the same moment.
Ramps are not available for coils and discrete inputs.

```scheme
(input 0 (type s16)
  (ramp (from 180) (to 240) (step 2) (every-ms 3000) (mode bounce)))
```

### What happens when a client writes

Clients can write `coil` and `holding` points. The written value is stored and returned by
later reads. If the point was a ramp, **it stops moving** and keeps the written value as a
static value until the simulator is restarted.

### `(count N)`

Repeats the point `N` times at consecutive addresses (the step is the point's width: 1 for
16-bit types, 2 for 32-bit types). All copies get the same value or ramp.

```scheme
(coil 0 (count 16))                         ; coils 0..15, all off
(holding 10 (type u32) (count 3) (static 7)) ; addresses 10-11, 12-13, 14-15
```

## `(fault ...)`

A device can have **one** fault. It applies to every response of that device, each time with
the chance given by `percent`.

| Entry | Meaning | Default |
|---|---|---|
| `(kind delay)` or `(kind drop)` | required. `delay` sends the response late, `drop` never sends it | - |
| `(delay-ms N)` | for `delay`: how late, 1-60000 ms. Required for `delay` | - |
| `(percent N)` | chance per response, 0-100 | `100` |

```scheme
(fault (kind delay) (delay-ms 1500) (percent 30))   ; 3 in 10 answers arrive 1.5 s late
(fault (kind drop) (percent 20))                    ; 1 in 5 answers never arrives
```

- Delays do not block the server: other clients are served while a response waits.
- A dropped response means the client sees a timeout. The write or read itself **still
  happens** on the device, like a real device whose answer got lost.
- Use `(seed N)` in `(server ...)` to repeat the exact same pattern of faults on every run.

## `(file ...)` (Read File Record, FC 20)

Defines 16-bit register records that a client can read with Modbus function code 20 (Read File
Record, 0x14). File records are separate from the coil/discrete/input/holding tables and are
read-only. If no file records are configured, any FC 20 read is unavailable (exception 02).

Shape:

```scheme
(file FILE_NO
  (record REC_NO VALUE ...)
  (record REC_NO VALUE ...))
```

- `FILE_NO` is the file number, 0-65535. A device can have several `(file ...)` entries (and
  several `(record ...)` entries inside a file).
- `REC_NO` is the starting record number, 0-9999 (`0x270F`).
- Each `VALUE` is a 16-bit register value, 0-65535. A record defines that many consecutive
  registers starting at `REC_NO` (one register per value, in order).
- Record ranges in the same file must not overlap. Record ranges must fit within record 9999.

```scheme
(file 1
  (record 0 0x1234 0x5678)   ; file 1, records 0-1
  (record 10 100 200 300))   ; file 1, records 10-12
(file 4
  (record 2 7))
```

A client request may read any contiguous range that is fully defined by configuration; requesting
an undefined file or any undefined record in the range returns exception 02 (illegal data
address), like undefined table addresses. Multiple sub-requests in one FC 20 request are
supported and answered in order; the request is rejected as a whole if any sub-request is
invalid or unavailable, or if the response would not fit in a Modbus PDU. See
[modbus-support.md](modbus-support.md) for the exact request/response format and exception
conditions. Config is limited to 16 files per device and 64 record ranges per file.

## Error messages

`modbus-sim --check FILE` reads the file, reports the first problem and exits. Messages say what
is wrong and show the entry. Common ones:

| Message | Meaning |
|---|---|
| `syntax error: missing close paren` | A `(` has no matching `)`. Check the end of the file and the entry before the reported text |
| `unknown option 'statik'. Allowed: ...` | Typo. The message lists the valid names |
| `address must be between 0 and 65535` | The address is out of range |
| `address is already used by another point` | Two points overlap. Remember 32-bit points use two addresses |
| `expected a number` / `expected a whole number` | A value has the wrong form, for example text in quotes |
| `missing required option (to ...)` | A ramp needs `from` and `to` |
| `coils and discrete inputs can only be (static on/off)` | No ramps and no `type` for bit tables |
| `a point cannot have both (static ...) and (ramp ...)` | Choose one |
| `ramp step must be greater than 0` | `step` is always positive; use `from` and `to` for the direction |
| `ramp range is too large for the step` | The ramp would need more than 2^63 steps; increase `step` or narrow `from`/`to` |
| `the file has more than one (server ...)` | Keep a single `(server ...)` block |
| `devices 'A' and 'B' both use unit-id N` | Give each device its own unit id |
| `device has more than one (fault ...)` | Only one fault per device |
| `unknown top-level form` | Only `(server ...)` and `(device ...)` are allowed at the top |

## A complete example

```scheme
; boiler.scm
(server (port 1502) (seed 1))

(device
  (name "Boiler controller")
  (unit-id 3)
  (note "Temperatures in 0.1 degrees C")

  (input 0 (type s16) (ramp (from 600) (to 850) (step 5) (every-ms 2000) (mode bounce)))
  (input 1 (type float32) (static 1.8))          ; pressure, bar (addresses 1 and 2)

  (holding 0 (type s16) (static 750))            ; setpoint, writable
  (coil 0 (static on))                           ; burner enable, writable
  (discrete 0 (static off))                      ; flame failure alarm

  (fault (kind delay) (delay-ms 400) (percent 10)))
```
