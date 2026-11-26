# Set a fault

Sets a fault of one kind on a device, replacing any fault of that kind.

## Syntax

```text
PUT /api/devices/{unit}/faults/{kind}
{"percent": N, ...}
```

## Default

`percent` 100

## Valid in

- `(control on)`
- kinds `delay`, `drop`, `exception`, `corrupt`, `disconnect`, `restart`, `sensor-disconnect`

## Behaviour

- The fields and rules are those of `(fault ...)` in the config. `percent` (0 to 100) applies to
  every kind. Kind-specific fields:

| Kind | Field |
|---|---|
| `delay` | `delay_ms`, 1 to 60000, required |
| `exception` | `exception_code`, one of 1 to 8, 10, 11, required |
| `corrupt` | `mode`: `pdu`, `register` or `checksum`, default `pdu` |

- `checksum` is refused in `tcp` and `udp`, and `disconnect` outside `tcp`, `rtu-tcp` and
  `ascii-tcp`, as in the config.
- Schedules cannot be set over the API: a PUT always fires by `percent`, and replaces a scheduled
  fault of the same kind from the config.
- Faults set here last until the process exits. A device `restart` fault resets the points and
  schedules but keeps them.

## Errors

| Status | When |
|---|---|
| 400 | malformed JSON, an unknown or misplaced field, or a missing required field |
| 404 | the unit or fault kind does not exist |
| 422 | a value is out of range or invalid for this transport |

## Examples

```bash
curl -s -X PUT -d '{"delay_ms":250,"percent":100}' http://127.0.0.1:9102/api/devices/1/faults/delay
curl -s -X PUT -d '{"exception_code":6}' http://127.0.0.1:9102/api/devices/1/faults/exception
```

```json
{"ok":true}
```

## See also

- [Clear a fault](api-clear-fault.md)
- [List faults](api-list-faults.md)
- [Faults on the wire](../../modbus/fault-effects.md)
- [Config reference: faults](../../config/faults/index.md)
