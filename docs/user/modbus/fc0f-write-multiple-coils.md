# Write Multiple Coils (0x0F)

Function code 0x0F (decimal 15) writes 1 to 1968 consecutive coils in one request.

## Syntax

```text
Request:   0F | start (2) | quantity (2) | byte count (1) | coil bytes (byte count)
Response:  0F | start (2) | quantity (2)
Exception: 8F | code (1)
```

## Valid in

- coil table
- all transports

## Behaviour

- `quantity` is 1 to 1968 (`0x0001` to `0x07B0`). `byte count` must be `ceil(quantity / 8)` and the
  request must be exactly `6 + byte count` bytes long.
- Bits are packed least significant bit first, like in [Read Coils (0x01)](fc01-read-coils.md).
- All-or-nothing: if any address is not configured, or any point in the range is read-only or driven
  by a machine, nothing is written.
- Writes last until the simulator restarts; matching machine `on-write` handlers run.

## Errors

| Code | When |
|---|---|
| 03 | `quantity` is 0 or above 1968, `byte count` does not match, or the request length is wrong |
| 02 | any address has no configured coil, a point is read-only, or a machine owns it |

## Examples

Switch coils 4 to 7 on:

```text
request:   0F 0004 0004 01 0F
response:  0F 0004 0004
```

```python
client.write_coils(4, [True, True, True, True])
```

## See also

- [Write Single Coil (0x05)](fc05-write-single-coil.md)
- [Writes are all-or-nothing](writes.md)
