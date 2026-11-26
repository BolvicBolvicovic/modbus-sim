# Mask Write Register (0x16)

Function code 0x16 (decimal 22) changes selected bits of one holding register with an AND mask and an OR mask.

## Syntax

```text
Request:   16 | address (2) | and mask (2) | or mask (2)
Response:  16 | address (2) | and mask (2) | or mask (2)   an echo of the request
Exception: 96 | code (1)
```

## Valid in

- holding table
- all transports

## Behaviour

- The new value is `(current AND and_mask) OR (or_mask AND (NOT and_mask))`. Any 16-bit masks are
  accepted.
- The current value is read at request time. Applying the result follows normal write rules: a ramp
  becomes static, machine `on-write` handlers run, and a point configured `(access read-only)` or driven
  by a machine refuses the write. The check happens before anything is read or written.
- The request must be exactly 7 bytes long.

## Errors

| Code | When |
|---|---|
| 03 | the request is not 7 bytes long |
| 02 | the address has no configured holding register, the point is read-only, or a machine owns it |

## Examples

Register 20 holds `0x0012`. With and mask `0x00F2` and or mask `0x0025` the result is `0x0017`:

```text
request:   16 0014 00F2 0025
response:  16 0014 00F2 0025
```

```python
client.mask_write_register(address=20, and_mask=0x00F2, or_mask=0x0025)
```

## See also

- [Write Single Register (0x06)](fc06-write-single-register.md)
- [Writes are all-or-nothing](writes.md)
