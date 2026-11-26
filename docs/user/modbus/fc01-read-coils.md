# Read Coils (0x01)

Function code 0x01 reads 1 to 2000 consecutive coils and returns them packed into bytes.

## Syntax

```text
Request:   01 | start (2) | quantity (2)
Response:  01 | byte count (1) | coil bytes (byte count)
Exception: 81 | code (1)
```

## Valid in

- coil table
- all transports

## Behaviour

- `start` and `quantity` are big-endian 16-bit numbers. `quantity` is 1 to 2000 (`0x0001` to `0x07D0`).
- The first coil is the least significant bit of the first byte, the next one the next bit, and so
  on. The last byte is padded with zeros; the byte count is `ceil(quantity / 8)`.
- Every address in the range must belong to a configured `(coil ...)` point. The range may span
  several points, but not a gap. Ramps, models and machines are evaluated when the request arrives.
- The request must be exactly 5 bytes long.

## Errors

| Code | When |
|---|---|
| 03 | `quantity` is 0 or above 2000, or the request is not 5 bytes long |
| 02 | any address in the range has no configured coil (or the range passes 65535) |
| 04 | a point in the range has a `(degradation (fail-after-ms ...))` that has passed |

## Examples

Read 8 coils from address 0. Coil 3 is on:

```text
request:   01 0000 0008
response:  01 01 08
```

```bash
./tools/modbus_probe.py coil 0 8
```

```python
from pymodbus.client import ModbusTcpClient

client = ModbusTcpClient("127.0.0.1", port=1502)
client.connect()
print(client.read_coils(0, count=8).bits)
```

## See also

- [Read Discrete Inputs (0x02)](fc02-read-discrete-inputs.md)
- [Write Single Coil (0x05)](fc05-write-single-coil.md)
- [Exception codes](exceptions.md)
