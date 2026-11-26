# `(device (canopen-object ...))`

Defines a CANopen object that FC 43 MEI `0x0D` SDO uploads and downloads read and write.

## Syntax

```scheme
(canopen-object NODE INDEX SUBINDEX "HEXBYTES")
```

## Default

none

## Valid in

- `(device ...)`

## Behaviour

Repeatable.

- `NODE` is the CANopen node id, 1-127.
- `INDEX` is 0-65535 and `SUBINDEX` is 0-255; both accept decimal or hex such as `#x6041`.
- `HEXBYTES` is 2-476 hex digits (1-238 bytes), with upper- or lowercase digits.

Each `(NODE, INDEX, SUBINDEX)` must be unique on a device, with at most 64 objects per device. Every
configured object is readable and writable through MEI `0x0D` SDO transfers, at any byte offset within
its configured data. Undefined objects and out-of-range transfers are rejected.

## Examples

```scheme
(device (unit-id 1)
  (canopen-object 5 #x6041 0 "17020000")
  (canopen-object 5 #x6041 1 "AABB"))
```

## See also

- [device-id-object](device-id-object.md)
- [Modbus support](../../modbus/modbus-overview.md)
