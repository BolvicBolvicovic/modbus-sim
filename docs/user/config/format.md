# Config file format

A config file describes the simulator: which transport and server options to use, and which devices
exist. Save it with any name, for example `my-device.scm`, and run `./mogbus my-device.scm`. To start
from a register map you already have (CSV or pymodbus), see [importing a register map](../import/import-overview.md).

## The format in one minute

The syntax is inspired by Scheme, but a config file is plain data: settings, not a program. These
rules cover all of it:

- Everything is in parentheses: `(name value)`. Every `(` needs a matching `)`.
- `;` starts a comment that runs to the end of the line.
- Text is in double quotes: `"Power meter"`.
- Numbers are written normally: `42`, `-5`, `0.5`, `230.4`.
- Hex numbers are written `#x1234` where examples use them.
- On/off can be written `on`, `off`, `yes`, `no`, `true`, `false`, `#t`, `#f`, `1` or `0`.
- Spaces, tabs and new lines are all the same. Indent however you like.
- Top-level forms and most entries can appear in any order. Exceptions: at most one `(server ...)`
  form; point address ranges cannot overlap; `(com-event ...)` entries are reported in the order
  written; and inside `(machine ...)`, `(initial ...)`, optional `(state-register ...)`, `(var ...)`,
  `(state ...)`, `(on-write ...)` and `(transition ...)` must be ordered as shown.

The file is only *read as data*. Nothing in it is run, so a typo can never do anything but
produce an error message.

## Overall shape

```scheme
(server ...)    ; optional, at most once

(device ...)    ; one or more
(device ...)
```

Anything else at the top level is rejected with `unknown top-level form`: only `(server ...)` and
`(device ...)` are allowed there. `mogbus --check FILE` reads the file, reports the first problem
and exits. Messages say what is wrong and show the entry; every entry page lists its own messages
under Errors, and the [error index](errors.md) lists them all.

## Errors

- `unknown top-level form`: only `(server ...)` and `(device ...)` are allowed at the top level of the file.

## How to read the reference

The reference has one page per key or form, grouped by where the key is written:

| Area | What is in it |
|---|---|
| [Server](server/index.md) | `(server ...)`: transport mode, listen address, serial line, timing, TLS and HTTP |
| [Device](device/index.md) | `(device ...)`: unit id, identification, diagnostics data, file records, FIFOs, function codes |
| [Points](points/index.md) | `(coil ...)`, `(discrete ...)`, `(input ...)`, `(holding ...)` and their data layout |
| [Behaviours](behaviours/index.md) | How a point's value moves: static, ramp, sine, lag, clock, noise, degradation |
| [Faults](faults/index.md) | `(fault ...)`: delays, drops, exceptions, corruption, disconnects, restarts |
| [Machines](machines/index.md) | `(machine ...)`: scripted state machines that drive points |

Each page starts with a one-sentence summary. After it, only the sections that apply appear, always
in this order: Syntax (the form), Default, Valid in (where the key may be written), Behaviour,
Errors (the messages `--check` prints for it), Examples and See also. In a Syntax line `A|B`
means one of the two, square brackets mean optional and `...` means more of the same.

## A complete example

```scheme
; boiler.scm
(server (mode tcp) (port 1502) (seed 1))

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
