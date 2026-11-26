# Write Multiple Registers (0x10)

Function code 0x10 (decimal 16) writes 1 to 123 consecutive holding registers in one request.

## Syntax

```text
Request:   10 | start (2) | quantity (2) | byte count (1) | values (2 x quantity)
Response:  10 | start (2) | quantity (2)
Exception: 90 | code (1)
```

## Valid in

- holding table
- all transports

## Behaviour

- `quantity` is 1 to 123 (`0x0001` to `0x007B`). `byte count` must be `2 x quantity` and the request
  must be exactly `6 + byte count` bytes long.
- All-or-nothing: if any address is not configured, or any point in the range is read-only or driven
  by a machine, nothing is written.
- Writing a register of a ramp freezes the owning point as a static value. Writes last until the
  simulator restarts; matching machine `on-write` handlers run.
- Use it to write 32-bit and 64-bit values in one piece, in the point's word and byte order.

## Errors

| Code | When |
|---|---|
| 03 | `quantity` is 0 or above 123, `byte count` is not `2 x quantity`, or the request length is wrong |
| 02 | any address has no configured holding register, a point is read-only, or a machine owns it |

## Examples

```text
request:   10 0000 0002 04 0001 0002
response:  10 0000 0002
```

```python
client.write_registers(0, [1, 2])
```

## See also

- [Write Single Register (0x06)](fc06-write-single-register.md)
- [Read/Write Multiple Registers (0x17)](fc17-read-write-multiple-registers.md)
- [Writes are all-or-nothing](writes.md)
