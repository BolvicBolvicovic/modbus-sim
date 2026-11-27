# Read Discrete Inputs (0x02)

Function code 0x02 reads 1 to 2000 consecutive discrete inputs and returns them packed into bytes.

## Syntax

```text
Request:   02 | start (2) | quantity (2)
Response:  02 | byte count (1) | input bytes (byte count)
Exception: 82 | code (1)
```

## Valid in

- discrete table
- all transports

## Behaviour

Same layout and limits as [Read Coils (0x01)](fc01-read-coils.md): quantity 1 to 2000, the first
input in the least significant bit of the first byte, a 5-byte request, every address configured.

Discrete inputs are read-only for Modbus clients: no function code writes them. A host can still
change them through the [C library](../library/library-simulate.md) or the
[control API](../observability/api/control-api.md).

A fired `(fault (kind sensor-disconnect))` turns a successful read into exception 04 and leaves the
connection open.

## Errors

| Code | When |
|---|---|
| 03 | `quantity` is 0 or above 2000, or the request is not 5 bytes long |
| 02 | any address in the range has no configured discrete input |
| 04 | a point in the range has a passed `fail-after-ms`, or a `sensor-disconnect` fault fired |

## Examples

```text
request:   02 0000 0008
response:  02 01 FF
```

```bash
./tools/modbus_probe.py discrete 0 8
```

```python
print(client.read_discrete_inputs(0, count=8).bits)
```

## See also

- [Read Coils (0x01)](fc01-read-coils.md)
- [Faults on the wire](fault-effects.md)
