# Write Single Coil (0x05)

Function code 0x05 switches one coil on or off.

## Syntax

```text
Request:   05 | address (2) | value (2)   value is FF00 (on) or 0000 (off)
Response:  05 | address (2) | value (2)   an echo of the request
Exception: 85 | code (1)
```

## Valid in

- coil table
- all transports

## Behaviour

- The value must be exactly `FF00` or `0000`. The request must be exactly 5 bytes long.
- The coil must be configured. A coil driven by a ramp keeps the written value from then on.
- A point configured `(access read-only)` and a point driven by a machine refuse the write.
- A successful write runs matching machine `on-write` handlers. Writes last until the simulator
  restarts; the config file is never changed.

## Errors

| Code | When |
|---|---|
| 03 | the value is neither `FF00` nor `0000`, or the request is not 5 bytes long |
| 02 | the address has no configured coil, the point is read-only, or a machine owns it |

## Examples

```text
request:   05 0003 FF00
response:  05 0003 FF00
```

A value that is not `FF00` or `0000`:

```text
request:   05 0003 00FF
response:  85 03
```

```bash
./tools/modbus_probe.py write-coil 3 on
```

```python
client.write_coil(3, True)
```

## See also

- [Write Multiple Coils (0x0F)](fc0f-write-multiple-coils.md)
- [Writes are all-or-nothing](writes.md)
