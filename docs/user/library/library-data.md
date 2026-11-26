# Data types and points

How `mogbus_point_t` describes a register or bit: types, byte and word order, strings and read-only points.

## Types

`mogbus_point_t.type` is one of `MOGBUS_TYPE_U16/S16/U32/S32/F32/BOOL` and, appended later,
`MOGBUS_TYPE_U64/S64/F64` (4 registers) and `MOGBUS_TYPE_STRING`.

- `word_order` applies to 32- and 64-bit types. `byte_order` (`MOGBUS_BYTES_BIG`, the zero default, or
  `MOGBUS_BYTES_LITTLE`) swaps the two bytes of every register of any register point. The two together
  give the ABCD, CDAB, BADC and DCBA layouts listed in the
  [config reference](../config/points/byte-order.md).
- Decoded access goes through a `double`, so `u64` and `s64` above 2^53 round in `value`,
  `mogbus_device_get_value()` and ramps; `mogbus_device_read/write` are exact.

## Strings

A string point has `type = MOGBUS_TYPE_STRING`, `length` registers (1 to 123) and the initial `text`
(copied; NULL means empty; at most `2 * length` bytes, padded with NUL). It must be in the holding or
input table.

- `mogbus_device_get_string()` copies the text up to its first NUL into your buffer (it fails when
  the buffer is smaller than the text plus the NUL).
- `mogbus_device_set_string()` replaces it, NUL padded. Unlike `mogbus_device_set_value()` it does not
  make the point host-owned: clients can still write the registers.
- `mogbus_device_get_value()` and `set_value()` return false on a string point.

## Read-only points

`read_only` (coils and holding registers; other spaces reject it) makes every Modbus write that
touches the point fail with exception 02, like `(access read-only)` in a config.
`mogbus_device_write()`, `mogbus_device_set_value()` and `set_string()` still change it.

## Compatibility

`byte_order`, `length`, `text` and `read_only` were appended to the struct, so code built against the
older header keeps working if it zero-initialises the struct.

## See also

- [C API overview](library-api.md)
- [Writes are all-or-nothing](../modbus/writes.md)
