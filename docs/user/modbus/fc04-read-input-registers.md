# Read Input Registers (0x04)

Function code 0x04 reads 1 to 125 consecutive 16-bit input registers.

## Syntax

```text
Request:   04 | start (2) | quantity (2)
Response:  04 | byte count (1) | register values (2 x quantity)
Exception: 84 | code (1)
```

## Valid in

- input table
- all transports

## Behaviour

Same layout and limits as [Read Holding Registers (0x03)](fc03-read-holding-registers.md): quantity
1 to 125, big-endian values, a 5-byte request, every address configured.

Input registers are read-only for Modbus clients. A fired `(fault (kind sensor-disconnect))` turns a
successful read into exception 04.

## Errors

| Code | When |
|---|---|
| 03 | `quantity` is 0 or above 125, or the request is not 5 bytes long |
| 02 | any address in the range has no configured input register |
| 04 | a point in the range has a passed `fail-after-ms`, or a `sensor-disconnect` fault fired |

## Examples

```text
request:   04 0000 0002
response:  04 04 00E6 00E7
```

```bash
./tools/modbus_probe.py input 0 2 --as float32
```

```python
print(client.read_input_registers(0, count=2).registers)
```

## See also

- [Read Holding Registers (0x03)](fc03-read-holding-registers.md)
- [Faults on the wire](fault-effects.md)
