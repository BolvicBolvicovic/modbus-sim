# `(holding (type string))`

Stores text in N registers of two ASCII characters each.

## Syntax

```scheme
(type string) (length N) [(padding nul|space)] [(static "TEXT")]
```

## Default

`padding nul`

## Valid in

- `(holding ...)`
- `(input ...)`

## Behaviour

`(length N)` is 1-123 registers; the first character is the high byte of the first register, so a
client sees it in reading order. `byte-order little` swaps the two characters inside every register.
`(static "TEXT")` sets the text, at most `2*N` bytes of printable ASCII (0x20-0x7E) in the config; a
shorter text is padded with NUL bytes, or with spaces when you add `(padding space)`.

Strings can only be `holding` or `input` points and only static: no `ramp`, `sine`, `lag`, `noise` or
`degradation`, no `word-order`. A client may write them (holding table) with raw register writes,
which changes the text until the device restarts; [`restart`](../faults/restart.md) restores the
configured text. `(count N)` repeats the whole string point. Machines cannot read, drive or watch
strings (config load fails). The C API and the Python package read and set the text with
`mogbus_device_get_string()`/`set_string()` (see [the C library](../../library/library-start.md)); `get_value` fails
on a string point. Text read back stops at the first NUL byte; a client may write any byte value.

## Errors

- `string points can only be (static "TEXT")`: strings are static; remove the ramp, sine or lag.
- `string points cannot have (noise ...) or (degradation ...)`: strings are static text, there is no value to disturb.
- `missing required option (length ...)`: a `(type string)` point needs `(length N)`, N registers.
- `(length ...): must be between 1 and 123`: a string is 1-123 registers (two characters each).
- `text is longer than N bytes (2 per register, length M)`: the `(static "TEXT")` does not fit its registers; raise `length`.
- `string text can only use printable ASCII (0x20-0x7E)`: use plain ASCII characters in a string's static text.
- `(length ...) and (padding ...) only apply to (type string)`: remove them from numeric points.
- `machine cannot drive a string point` / `a string point has no number to read` / `state register cannot name a string point` / `on-write cannot watch a string point`: machines work on numbers; use a numeric point.

## Examples

```scheme
(device (unit-id 1)
  (input 20 (type string) (length 8) (static "SN-000417"))              ; 16 bytes, NUL padded
  (input 28 (type string) (length 8) (padding space) (static "EM-100")) ; space padded
  (holding 40 (type string) (length 4)))                                ; empty, writable
```

Read the text back:

```bash
./tools/modbus_probe.py input 20 8 --as string
```

## See also

- [type](type.md)
- [byte-order](byte-order.md)
