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
| `(idle-timeout-ms N)` | Close a client connection after `N` milliseconds with no I/O progress, 0-86400000. `0` disables the timeout. A connection with a delayed response pending is not considered idle | `0` (disabled) |
| `(baud N)` | RTU/RTU-over-TCP: standard termios serial baud rate (e.g. 9600, 19200, 38400, 115200). When known, Modbus RTU t3.5 inter-frame timing is derived from this baud | `0` (unknown) |
| `(frame-gap-ms N)` | RTU/RTU-over-TCP: explicit inter-frame silence override in milliseconds, 1-60000. Takes precedence over baud-derived t3.5 timing. If omitted and baud is unknown, RTU falls back to 20 ms | `0` (auto) |
| `(strict-t15 on\|off)` | RTU byte streams: opt-in best-effort t1.5 discard heuristic in baud-derived auto mode. Not supported for RTU-over-TCP | `off` |

The command-line options `--port` and `--bind` override server listen settings for TCP and RTU-over-TCP. RTU timing can be
overridden with `--baud` / `--rtu-baud`, `--frame-gap-ms`, and `--strict-t15`; `--frame-gap-ms` takes precedence over
baud-derived timing when both are provided. `strict-t15` is never applied to RTU-over-TCP. For tty devices, if no
explicit baud is configured, `modbus-sim` reads the existing tty baud rate for t3.5 timing.

## `(device ...)`

| Entry | Meaning | Default |
|---|---|---|
| `(name "Text")` | Name shown in logs and `--check`, up to 63 characters | `"unnamed"` |
| `(unit-id N)` | Modbus unit id (slave address). TCP: 0-255. RTU: 1-247 (`0` is broadcast, `248-255` reserved by Modbus serial spec) | `1` |
| `(exception-status N)` | Byte returned by Read Exception Status (FC 07), 0-255 | `0` |
| `(server-id N)` | Server ID byte returned by Report Server ID (FC 17), 0-255 | `0` |
| `(run-indicator N)` | Run Indicator Status byte returned by Report Server ID (FC 17), 0-255 (`0xFF` ON/run, `0x00` OFF by convention) | `255` |
| `(com-status N)` | Status returned by Get Com Event Counter (FC 11) and Get Com Event Log (FC 12), `0` (ready) or `65535` (busy) | `0` |
| `(com-event N)` | One FC 12 event byte, 0-255. Repeatable up to 64 entries; reported in configuration order | none (empty log) |
| `(vendor-name "Text")` | MEI 14 Read Device Identification object 0x00, 0-244 bytes | `"modbus-sim"` |
| `(product-code "Text")` | MEI 14 object 0x01, 0-244 bytes | `"modbus-sim"` |
| `(major-minor-revision "Text")` | MEI 14 object 0x02, 0-244 bytes | `"1.0"` |
| `(vendor-url "Text")` | MEI 14 object 0x03, 0-244 bytes; setting any regular object reports regular conformity (0x82) | absent |
| `(product-name "Text")` | MEI 14 object 0x04, 0-244 bytes | absent |
| `(model-name "Text")` | MEI 14 object 0x05, 0-244 bytes | absent |
| `(user-application-name "Text")` | MEI 14 object 0x06, 0-244 bytes | absent |
| `(note "Text")` | A comment kept for humans. Can be repeated. Ignored by the program | - |
| `(fault ...)` | One fault, see below | none |
| `(file ...)` | File record data for Read/Write File Record (FC 20/21), see below | none |
| `(fifo ...)` | FIFO queue data for Read FIFO Queue (FC 24), see below | none |
| `(coil ...)` `(discrete ...)` `(input ...)` `(holding ...)` | Data points, see below | - |

Unit ids must be different between devices. In RTU mode, unit ids 0 and 248-255 are rejected at
startup and by `--check` when RTU flags are given because address 0 is broadcast and 248-255 are
reserved by the Modbus serial specification.

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
Integer register values are rounded to the nearest whole number and limited to the range of the type.
`float32` can represent `NaN`, `+inf.0`, and `-inf.0` (useful as invalid-sensor sentinels). Finite
`float32` values outside the representable float range become `+inf.0`/`-inf.0` instead of causing
undefined behavior. Non-finite static values are accepted only for `float32`; integer and on/off
static values must be finite.

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
16-bit types, 2 for 32-bit types). All copies get the same value or ramp. A device can have at
most 8192 points total (`DEVICE_MAX_POINTS`); a count that would exceed this is rejected.

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

## `(file ...)` (File Records, FC 20/21)

Defines 16-bit register records that a client can read with Modbus function code 20 (Read File
Record, 0x14) and, in this simulator, write with Modbus function code 21 (Write File Record,
0x15). File records are separate from the coil/discrete/input/holding tables. Only the ranges
configured here exist and are writable/readable (defaults to none): FC 20/21 requests targeting an
undefined file or any undefined record return exception 02. If no file records are configured, any
FC 20 read or FC 21 write is unavailable (exception 02). FC 21 updates in-memory values (readable
back via FC 20) until the simulator restarts; it does not modify the config file and does not
create new files/records.

Shape:

```scheme
(file FILE_NO
  (record REC_NO VALUE ...)
  (record REC_NO VALUE ...))
```

- `FILE_NO` is the file number, 1-65535 (`0x0001-0xFFFF`). A device can have several `(file ...)`
  entries (and several `(record ...)` entries inside a file).
- `REC_NO` is the starting record number, 0-9999 (`0x270F`).
- Each `VALUE` is a 16-bit register value, 0-65535. A record defines that many consecutive
  registers starting at `REC_NO` (one register per value, in order).
- Record ranges in the same file must not overlap. Record ranges must fit within record 9999.

```scheme
(file 1
  (record 0 #x1234 #x5678)   ; file 1, records 0-1
  (record 10 100 200 300))   ; file 1, records 10-12
(file 4
  (record 2 7))
```

A client request may read or write any contiguous range that is fully defined by configuration;
requesting an undefined file or any undefined record in the range returns exception 02 (illegal data
address), like undefined table addresses. Multiple sub-requests in one FC 20/21 request are
supported and answered in order (FC 21 echoes the request on success); the request is rejected as a
whole if any sub-request is invalid or unavailable/unwritable, or (for FC 20) if the response would
not fit in a Modbus PDU. FC 21 is all-or-nothing: no records are modified if any sub-request fails
validation. See [modbus-support.md](modbus-support.md) for the exact request/response format,
semantics, and exception conditions. Config is limited to 16 files per device and 64 record ranges
per file.

## `(fifo ...)` (Read FIFO Queue, FC 24)

Defines a FIFO/queue that a client can read with Modbus function code 24 (Read FIFO Queue, 0x18).
FIFO queues are separate from the coil/discrete/input/holding tables and file records. Only the
FIFO pointer addresses configured here exist (defaults to none): an FC 24 request for an undefined
FIFO pointer returns exception 02. If no FIFO queues are configured, FC 24 is unavailable
(exception 02).

Shape:

```scheme
(fifo ADDRESS (values VALUE ...))
```

- `ADDRESS` is the FIFO pointer address, 0-65535. Each `(fifo ...)` must use a different address.
- `VALUE` is a 16-bit register value, 0-65535. Values are returned by FC 24 in the order listed.
  A FIFO may contain 0 to 31 values (an empty queue is `(fifo ADDRESS (values))`).

```scheme
(fifo 100 (values #x1234 #x5678 42))
(fifo 200 (values))                      ; empty queue: FIFO Count 0
```

FC 24 reads are **non-destructive**: reading a FIFO does not change its contents or count, so
repeated reads return the same values. FIFO contents are config data (fixed for the run; not
writable via Modbus and not modified by the simulator). The response `FIFO Count` is the number of
configured values and is always at most 31. Config is limited to 16 FIFO queues per device. See
[modbus-support.md](modbus-support.md) for the exact request/response format and exception
conditions.

## Serial communication status and event log (FC 11/12)

```scheme
(com-status N)      ; N is 0 (ready) or 65535 (busy); default 0
(com-event N)       ; one event byte, 0-255; repeat up to 64 times; default empty
```

```scheme
(device
  (com-status 0)
  (com-event 128)
  (com-event 255)
  (holding 0))
```

`com-status` is the status field returned by both FC 11 and FC 12. Any value other than `0` or
`65535` is rejected by `--check` and at startup.

Each `com-event` supplies one byte of the FC 12 Events field, in the order written. Values must be
0-255 and at most 64 entries are allowed; a 65th entry is rejected. FC 12 Byte Count is
`6 + number of event bytes`, so an empty log gives Byte Count `6` and an 8-byte PDU, and 64 events
give Byte Count `70`. The simulator reports the configured bytes verbatim and does not interpret
serial-line event nibbles (it does not split an event byte into transmitter/receiver status or
decode its meaning). The log is static for the run; FC 08 does not clear it.

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
| `ramp range is too large for the step` | The ramp would need 2^63 steps or more; increase `step` or narrow `from`/`to` |
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
