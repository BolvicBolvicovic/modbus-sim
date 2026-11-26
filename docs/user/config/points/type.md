# `(holding (type ...))`

Sets how a register point is encoded: integer, float or string, in one to four registers.

## Syntax

```scheme
(type u16|s16|u32|s32|float32|u64|s64|float64|string)
```

## Default

`u16`

## Valid in

- `(holding ...)`
- `(input ...)`

## Behaviour

| Type | Registers used | Range |
|---|---|---|
| `u16` | 1 | 0 to 65535 |
| `s16` | 1 | -32768 to 32767 |
| `u32` | 2 | 0 to 4294967295 |
| `s32` | 2 | -2147483648 to 2147483647 |
| `float32` | 2 | IEEE 754 single precision |
| `u64` | 4 | 0 to 18446744073709551615 |
| `s64` | 4 | -9223372036854775808 to 9223372036854775807 |
| `float64` | 4 | IEEE 754 double precision |
| `string` | `(length N)` | N registers of text, see [strings](string.md) |

Integer register values are rounded to the nearest whole number and limited to the range of the type.
`float32` and `float64` can represent `NaN`, `+inf.0`, and `-inf.0` (useful as invalid-sensor
sentinels). Finite `float32` values outside the representable float range become `+inf.0`/`-inf.0`
instead of causing undefined behavior. Non-finite static values are accepted only for the float
types; integer and on/off static values must be finite.

A multi-register point occupies consecutive addresses: `(input 4 (type u32) ...)` uses addresses 4
and 5, `(input 4 (type u64) ...)` uses 4 to 7.

**64-bit precision.** Values are doubles inside mogbus (ramps, models, machines, `get_value`), so
`u64` and `s64` integers above 2^53 (about 9.007e15) lose precision there. Raw register reads and
writes by clients are exact, and so is a `(static N)` integer: s7 reads integers as 64-bit, so
`(static 9223372036854775807)` and `(static -9223372036854775808)` encode exactly. A `u64` static
above 2^63-1 must be written in hex, `(static #xFFFFFFFFFFFFFFFF)` (s7 has no decimal literal for
it); for `u64` an integer literal is taken as 64-bit two's complement, so a negative one wraps
instead of clamping. A static with `(noise ...)` or `(degradation ...)` is a model and goes through
doubles.

## Examples

```scheme
(device (unit-id 1)
  (input 0 (type s16) (static -40))
  (input 2 (type float32) (static 0.97))       ; addresses 2 and 3
  (input 4 (type u64) (static #xFFFFFFFFFFFFFFFF))  ; addresses 4 to 7
  (holding 0 (type float32) (static +inf.0)))  ; invalid-sensor sentinel
```

Read the float as one value:

```bash
./tools/modbus_probe.py input 2 2 --as float32
```

## See also

- [word-order](word-order.md)
- [byte-order](byte-order.md)
- [strings](string.md)
