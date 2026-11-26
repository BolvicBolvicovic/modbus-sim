# Read/Write Multiple Registers (0x17)

Function code 0x17 (decimal 23) writes a range of holding registers and then reads a range of holding registers in one transaction.

## Syntax

```text
Request:   17 | read start (2) | read quantity (2) | write start (2) | write quantity (2) | write byte count (1) | write values (2 x write quantity)
Response:  17 | byte count (1) | read values (2 x read quantity)
Exception: 97 | code (1)
```

## Valid in

- holding table
- all transports

## Behaviour

- Read quantity is 1 to 125 (`0x007D`), write quantity 1 to 121 (`0x0079`), and write byte count must be
  `2 x write quantity`. The request is `10 + write byte count` bytes long.
- The write happens before the read, so the read result shows the new values, also where the two
  ranges overlap.
- Both ranges must be fully configured holding registers; they are checked before anything is
  written. The write range must also be writable: a point configured `(access read-only)` or driven by a
  machine refuses the whole request. On any exception no register changes.
- Writes follow the holding register rules: ramps freeze to static, `on-write` handlers run.
- A read range that covers a failed point (`fail-after-ms` passed) gives exception 04.

## Errors

| Code | When |
|---|---|
| 03 | the request is too short or the wrong length, a quantity is out of range, or write byte count is not `2 x write quantity` |
| 02 | any address of either range is not configured, or the write range is read-only or machine-driven |
| 04 | the read range covers a failed point |

## Examples

Write `0x00FF` to register 0 and read registers 0 and 1:

```text
request:   17 0000 0002 0000 0001 02 00FF
response:  17 04 00FF 0002
```

```python
client.readwrite_registers(address=0, read_count=2, write_address=0, values=[0x00FF])
```

## See also

- [Read Holding Registers (0x03)](fc03-read-holding-registers.md)
- [Write Multiple Registers (0x10)](fc10-write-multiple-registers.md)
- [Writes are all-or-nothing](writes.md)
