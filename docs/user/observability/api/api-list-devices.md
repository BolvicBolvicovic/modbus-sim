# List devices

Lists the configured devices with their point counts per table.

## Syntax

```text
GET /api/devices
```

## Valid in

- `(control on)`

## Behaviour

- Returns one object per device: `unit`, `name` and `points`, the number of configured points in
  each of the tables `coil`, `discrete`, `input` and `holding`.
- No request body.

## Errors

| Status | When |
|---|---|
| 404 | the control API is off |
| 405 | the method is not GET |

## Examples

```bash
curl -s http://127.0.0.1:9102/api/devices
```

```json
[{"unit":1,"name":"Meter","points":{"coil":0,"discrete":0,"input":0,"holding":2}}]
```

## See also

- [Runtime control API](control-api.md)
- [Read values](api-read-values.md)
