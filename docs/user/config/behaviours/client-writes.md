# What happens when a client writes

Clients can write `coil` and `holding` points; a written value replaces the point's behaviour until the device restarts.

## Behaviour

The written value is stored and returned by later reads. If the point was a ramp, sine or lag (or had
noise or degradation), **it stops moving** and keeps the written value as a static value until the
simulator is restarted. Machine-owned points, including state registers, reject client writes with
exception 02 (see [machine points](../machines/points.md)). A point marked
[`(access read-only)`](../points/access.md) rejects every client write with exception 02 and keeps
moving. The `fail-after-ms` failure of a [degradation](degradation.md) survives a write.

## Examples

```bash
./tools/modbus_probe.py holding 0 1
./tools/modbus_probe.py write-reg 0 1234
./tools/modbus_probe.py holding 0 1   # now 1234 for good, even if the point was a ramp
```

## See also

- [access](../points/access.md)
- [restart](../faults/restart.md)
