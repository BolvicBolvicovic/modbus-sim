# CANopen General Reference (0x2B / 0x0D)

Function code 0x2B with MEI type 0x0D reads and writes CANopen object dictionary entries (SDO upload and download).

## Syntax

```text
Request:   2B | 0D | control (1) | 00 | network ID (0-1) | data type (0-1) | node (1) | index (2) | subindex (1) | start (2) | count (2) | data (write only)
Response:  2B | 0D | control (1) | 00 | network ID (0-1) | node (1) | index (2) | subindex (1) | start (2) | count (2) | data (read only)
Failure:   AB | FF | length (2) | 0D | AE or CE | payload
```

```scheme
(canopen-object NODE INDEX SUBINDEX "HEXBYTES")
```

## Default

No objects: every transfer fails with `CE 0xFFFF000B`.

## Valid in

- CANopen objects
- all transports

## Behaviour

- Multi-byte fields are big-endian. Control byte bits: `0x01` write (download) instead of read
  (upload), `0x04` a network ID byte follows (and is echoed), `0x02` a data-type byte follows.
- A read (upload) request has no data and `count >= 1`; the response carries `count` bytes. A write
  (download) request carries exactly `count` bytes, and its response has `count` set to `0000`.
- The reserved byte after the control byte must be 0.
- Only plain unsigned 8-bit data is implemented. The data-type bit (`0x02`) and the extend (`0x80`),
  counter (`0x20`) and extended-control (`0x40`) bits produce the `AE` failure.
- Every configured object can be read and written at any offset within its data. Only object
  dictionary access exists: PDO, NMT, LSS and extended-fragment services are not implemented.
- Command addressing (`node = 0x7F`, `index = 0xFFFF`) is recognized but always answered with
  `CE 0xFFFF1003`, because there is no CAN gateway or virtual node.
- In RTU the request length is derived from the count field; known CiA data types (Unsigned8
  `0x00`, Unsigned16 `0x01`, Unsigned24 `0x02`, Unsigned32 `0x03`, Integer16 `0x11`, Integer32
  `0x13`, Real32 `0x20`, Real64 `0x21`, Visible string `0x50`, Domain `0x60`; unknown values count one
  byte) are used only for that, so a request that selects encoded data can still be answered with `AE`.

### Extended failures

`AB FF len 0D AE|CE payload`: `len` counts the MEI type, the failure byte and the payload.

| Failure | Payload | Meaning |
|---|---|---|
| `AE` | supported control byte `0x04 \| (0x01 if write)`, length `0003` | an unsupported option was requested |
| `CE` | 4-byte little-endian code, length `0006` | the service failed |

| `CE` code | Meaning |
|---|---|
| `0xFFFF000B` | the object does not exist |
| `0xFFFF0011` | invalid count or length (`count = 0`, or `count` does not match the data) |
| `0x06070012` | the requested length exceeds the configured object |
| `0x08000020` | an [access hook](../library/library-hooks.md) refused the read or write (library only) |
| `0xFFFF001E` | the response is too large for a Modbus PDU |
| `0xFFFF1003` | the service is not supported (command addressing) |

## Errors

| Code | When |
|---|---|
| 03 | the MEI 0x0D header is truncated, or the reserved byte is not 0 |
| 01 | the MEI type is neither `0x0D` nor `0x0E` |

Every other failure is one of the extended `AB FF` responses above.

## Examples

Read 4 bytes of node 5, object `0x6041`, subindex 0, with `(canopen-object 5 #x6041 0 "17020000")`:

```text
request:   2B 0D 00 00 05 6041 00 0000 0004
response:  2B 0D 00 00 05 6041 00 0000 0004 17020000
```

An object that does not exist (`CE`, `0xFFFF000B` little-endian):

```text
response:  AB FF 0006 0D CE 0B 00 FF FF
```

## See also

- [Read Device Identification (0x2B / 0x0E)](fc2b-0e-read-device-identification.md)
- [Config reference: CANopen objects](../config/device/canopen-object.md)
