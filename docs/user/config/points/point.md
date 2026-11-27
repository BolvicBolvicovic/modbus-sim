# `(coil|discrete|input|holding ADDRESS ...)`

Defines a point in one of the four Modbus tables, at an address a client can read or write.

## Syntax

```scheme
(holding ADDRESS
  (type ...)          ; registers only, default u16
  (word-order ...)    ; 32- and 64-bit types only, default high-first
  (byte-order ...)    ; registers only, default big
  (length N)          ; (type string) only: registers, 1-123
  (padding ...)       ; (type string) only: nul (default) or space
  (static VALUE)      ; or (ramp ...), (sine ...), (lag ...) or (clock FIELD): only one. None: 0 / off
  (noise ...)         ; optional, registers only: added to any of the above
  (degradation ...)   ; optional: drift, noise growth, stuck and fail over time
  (access ...)        ; coil and holding only: read-write (default) or read-only
  (count N)           ; optional: repeat for N consecutive points
  (note "Text"))      ; optional comment
```

## Default

a point with no behaviour holds `0` (or off)

## Valid in

- `(device ...)`

## Behaviour

There are four Modbus tables. Use the one your real device uses.

| Entry | Table | Holds | Can a client write it? | Function codes |
|---|---|---|---|---|
| `coil` | Coils | on/off | yes | 01, 05, 15 |
| `discrete` | Discrete inputs | on/off | no | 02 |
| `input` | Input registers | numbers | no | 04 |
| `holding` | Holding registers | numbers | yes | 03, 06, 16, 22, 23 |

`ADDRESS` is the Modbus address as the client sends it on the wire (0-65535). If your device's manual
says "40001", that is address `0` of the holding table; "40101" is `100`.

**Addresses you do not define do not exist.** A client reading or writing one gets exception 02
(illegal data address), like a real device. A read that covers a gap also fails. Each table has 65536
addresses; a device can have at most 8192 points in total (`DEVICE_MAX_POINTS`).

Coils and discrete inputs are always on/off; do not give them a `type`. They can only be
`(static on/off)`.

## Errors

- `address must be between 0 and 65535`: the address is out of range.
- `address is already used by another point`: two points overlap. Remember 32-bit points use two addresses, 64-bit points four and strings `length`.
- `unknown option 'statik'. Allowed: ...`: typo. The message lists the valid names.
- `expected a number` / `expected a whole number`: a value has the wrong form, for example text in quotes.
- `coils and discrete inputs can only be (static on/off)`: no ramps and no `type` for bit tables.
- `syntax error: missing close paren`: a `(` has no matching `)`. Check the end of the file and the entry before the reported text.

## Examples

```scheme
(device (unit-id 1)
  (coil 0 (static on))
  (discrete 0 (static off))
  (input 0 (type u16) (static 230))
  (holding 0 (type u16) (static 1200)))
```

Read the first address, then one that is not defined:

```bash
./tools/modbus_probe.py holding 0 1
./tools/modbus_probe.py holding 5 1   # no point at 5: exception 02
```

## See also

- [type](type.md)
- [access](access.md)
- [count](count.md)
- [behaviours](../behaviours/index.md)
