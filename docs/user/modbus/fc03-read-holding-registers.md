# Read Holding Registers (0x03)

Function code 0x03 reads 1 to 125 consecutive 16-bit holding registers.

## Syntax

```text
Request:   03 | start (2) | quantity (2)
Response:  03 | byte count (1) | register values (2 x quantity)
Exception: 83 | code (1)
```

## Valid in

- holding table
- all transports

## Behaviour

- `quantity` is 1 to 125 (`0x0001` to `0x007D`); the byte count is `2 x quantity`. Values are big-endian.
- Every address in the range must belong to a configured `(holding ...)` point. The range may span
  several points, but not a gap.
- A 32-bit or 64-bit value occupies several registers; their order follows the point's
  `word-order` and `byte-order`. A client must read all of them in one request to get a coherent
  value.
- Values that depend on time (ramps, models, machines) are computed when the request arrives.
- The request must be exactly 5 bytes long.

## Errors

| Code | When |
|---|---|
| 03 | `quantity` is 0 or above 125, or the request is not 5 bytes long |
| 02 | any address in the range has no configured holding register |
| 04 | a point in the range has a passed `fail-after-ms` |

## Examples

Read two registers from address 0, both holding 42:

```text
request:   03 0000 0002
response:  03 04 002A 002A
```

An unconfigured address:

```text
request:   03 00FF 0001
response:  83 02
```

```bash
./tools/modbus_probe.py holding 0 2
./tools/modbus_probe.py holding 100 2 --as float32
```

```python
print(client.read_holding_registers(0, count=2).registers)
```

## See also

- [Read Input Registers (0x04)](fc04-read-input-registers.md)
- [Write Single Register (0x06)](fc06-write-single-register.md)
- [Read/Write Multiple Registers (0x17)](fc17-read-write-multiple-registers.md)
- [Config reference: data points](../config/points/index.md)
