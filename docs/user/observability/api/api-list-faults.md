# List faults

Lists the faults that are set on a device.

## Syntax

```text
GET /api/devices/{unit}/faults
```

## Valid in

- `(control on)`

## Behaviour

- Returns the faults of the device itself, one object per kind with `kind`, `percent`, `scheduled`
  and the kind's own fields (`delay_ms`, `exception_code`, `mode`).
- Faults that a machine state adds are not listed.

## Errors

| Status | When |
|---|---|
| 404 | the unit does not exist |

## Examples

```bash
curl -s http://127.0.0.1:9102/api/devices/1/faults
```

```json
{"faults":[{"kind":"delay","percent":100,"scheduled":false,"delay_ms":250}]}
```

## See also

- [Set a fault](api-set-fault.md)
- [Clear a fault](api-clear-fault.md)
