# Exception codes

When mogbus cannot carry out a request it answers with an exception: the function code with bit 7 set, followed by one exception code. Most codes come from the request itself; the others exist only for fault injection.

## The codes

| Code | Name | When |
|---|---|---|
| 01 | Illegal Function | the function code is not one of the [function codes](function-codes.md) |
| 02 | Illegal Data Address | any address of the request is not defined in the config (or is above 65535), a write targets a machine-owned point such as a state register, or a write touches a point configured `(access read-only)` |
| 03 | Illegal Data Value | quantity is 0 or above the limit, the request has the wrong length or byte count, or a single-coil value is not `FF00` / `0000` |
| 04 | Slave Device Failure | FC 01/02/03/04/23 and user-defined `(registers ...)` reads covering a point whose `(degradation (fail-after-ms ...))` has passed; successful FC 02/04 reads via `(fault (kind sensor-disconnect))`; any response via `(fault (kind exception) (code 4))` |
| 05 | Acknowledge | only via `(fault (kind exception) (code 5))` |
| 06 | Slave Device Busy | only via `(fault (kind exception) (code 6))` |
| 07 | Negative Acknowledge | only via `(fault (kind exception) (code 7))` |
| 08 | Memory Parity Error | only via `(fault (kind exception) (code 8))` |
| 09 | reserved | never emitted and rejected by config |
| 0A (10) | Gateway Path Unavailable | only via `(fault (kind exception) (code 10))` |
| 0B (11) | Gateway Target Device Failed to Respond | no device has the requested unit id; also available via `(fault (kind exception) (code 11))` |

Codes 01 to 04 and 0B come from the simulator's own checks. Codes 05 to 08 and 0A are never produced
by validation: they exist only for fault injection. Code 04 is also produced by a fired
`sensor-disconnect` fault. Fault injection is a simulator testing feature, not part of the Modbus
protocol.

## Order of checks

1. The function code (01).
2. The request shape and the quantity (03).
3. The addresses (02).

So a request with a bad quantity and an undefined address answers 03.

## Extended failures

FC 0x2B with MEI type 0x0D answers CANopen failures in an extended form, `AB FF len 0D AE|CE payload`,
described in [CANopen General Reference (0x2B / 0x0D)](fc2b-0d-canopen-general-reference.md).

## Unit ids

Exception 0B answers a request for a unit id that no device has, in the MBAP transports. The serial
and RTU/ASCII-framed transports stay silent instead. See [Unit ids and broadcast](unit-ids.md).

## See also

- [Faults on the wire](fault-effects.md)
- [Config reference: faults](../config/faults/index.md)
