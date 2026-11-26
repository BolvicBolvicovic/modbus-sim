# `(device (device-id-object ...))`

Adds an extended Read Device Identification object (FC 43, MEI 14).

## Syntax

```scheme
(device-id-object ID "Text")
```

## Default

none

## Valid in

- `(device ...)`

## Behaviour

Repeatable, in any order. `ID` must be a decimal integer from 128 to 255 (`0x80-0xFF`); text must be
non-empty and at most 244 bytes. IDs must be unique and at most 32 extended objects may be configured
per device; entries are served in ascending ID order. Configuring any extended object raises FC 43
Conformity Level to `0x83`. MEI `0x0E` is the Read Device Identification type; FC 43 with MEI `0x0D`
performs CANopen SDO transfers against [`(canopen-object ...)`](canopen-object.md) data. See
[Modbus support](../../modbus-support.md) for response semantics.

## Examples

```scheme
(device (unit-id 1)
  (holding 0)
  (device-id-object 128 "First extended object")
  (device-id-object 255 "Last extended object"))
```

## See also

- [identification objects](identification.md)
- [canopen-object](canopen-object.md)
