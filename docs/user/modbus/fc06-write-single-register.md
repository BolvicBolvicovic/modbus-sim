# Write Single Register (0x06)

Function code 0x06 writes one 16-bit holding register.

## Syntax

```text
Request:   06 | address (2) | value (2)
Response:  06 | address (2) | value (2)   an echo of the request
Exception: 86 | code (1)
```

## Valid in

- holding table
- all transports

## Behaviour

- Any 16-bit value is accepted. The request must be exactly 5 bytes long.
- The register must be configured. Writing a register of a ramp freezes the owning point as a static
  value.
- A point configured `(access read-only)` and a point driven by a machine refuse the write.
- Matching machine `on-write` handlers run after the value is applied. Writes last until the
  simulator restarts.

## Errors

| Code | When |
|---|---|
| 03 | the request is not 5 bytes long |
| 02 | the address has no configured holding register, the point is read-only, or a machine owns it |

## Examples

```text
request:   06 001E 04D2
response:  06 001E 04D2
```

```bash
./tools/modbus_probe.py write-reg 30 1234
```

```python
client.write_register(30, 1234)
```

## See also

- [Write Multiple Registers (0x10)](fc10-write-multiple-registers.md)
- [Mask Write Register (0x16)](fc16-mask-write-register.md)
- [Writes are all-or-nothing](writes.md)
