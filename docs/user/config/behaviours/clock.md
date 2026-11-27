# `(clock FIELD)`

Makes a point show one field of the wall clock of the machine running the simulator, read at request time.

## Syntax

```scheme
(clock year|month|day|weekday|hour|minute|second|unix [(utc on|off)])
```

## Default

Calendar fields use local time (`(utc off)`). `unix` is always UTC.

## Valid in

- `(input ...)`
- `(holding ...)`

## Behaviour

| Field | Value |
|---|---|
| `year` | for example 2026 |
| `month` | 1-12 |
| `day` | 1-31 |
| `weekday` | ISO: Monday 1 ... Sunday 7 |
| `hour` | 0-23 |
| `minute` | 0-59 |
| `second` | 0-59 |
| `unix` | seconds since 1970-01-01 00:00:00 UTC |

The point needs an integer type (`u16`, `s16`, `u32`, `s32`, `u64` or `s64`); `unix` needs at least 32
bits. Local time follows the `TZ` of the process. A client write freezes the point at the written value
like a ramp (holding registers only), and a device restart makes it live again. A machine cannot drive a
clock point, and `noise` and `degradation` do not apply to it.

A device with several clock points reads the clock once per request, so the fields of one request agree
with each other.

## Errors

- `unknown clock field 'X'`: use one of the eight field names.
- `clock unix needs a 32-bit or 64-bit type (u32, s32, u64 or s64)`: `unix` does not fit in 16 bits.
- `clock points need an integer register type (u16, s16, u32, s32, u64 or s64)`: not for floats.
- `coils and discrete inputs can only be (static on/off)`: no clock for bit tables.

## Examples

```scheme
(device (unit-id 1)
  (holding 100 (clock year))
  (holding 101 (clock month))
  (holding 102 (clock day))
  (holding 103 (clock hour))
  (holding 104 (clock minute))
  (holding 105 (clock second))
  (holding 106 (clock hour (utc on)))
  (input 200 (type u32) (clock unix)))
```

## See also

- [ramp](ramp.md)
- [client writes](client-writes.md)
