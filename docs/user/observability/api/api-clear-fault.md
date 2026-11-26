# Clear a fault

Removes the fault of one kind from a device.

## Syntax

```text
DELETE /api/devices/{unit}/faults/{kind}
```

## Valid in

- `(control on)`

## Behaviour

- Removes a fault set over the API or configured in the file.
- Answers 404 when no fault of that kind is set.

## Errors

| Status | When |
|---|---|
| 404 | the unit or kind does not exist, or the fault is not set |

## Examples

```bash
curl -s -X DELETE http://127.0.0.1:9102/api/devices/1/faults/delay
```

```json
{"ok":true}
```

```json
{"error":"fault is not set"}
```

## See also

- [Set a fault](api-set-fault.md)
- [List faults](api-list-faults.md)
