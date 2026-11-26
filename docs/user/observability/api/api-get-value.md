# Read a decoded value

Reads the decoded value of the point that starts at an address.

## Syntax

```text
GET /api/devices/{unit}/{space}/{address}/value
```

## Valid in

- `(control on)`
- all four tables

## Behaviour

- Returns `{"value":N}`: the decoded number of the point whose first address is `address`, for
  example a `float32` of two registers.
- The address must be the first address of a point.

## Errors

| Status | When |
|---|---|
| 404 | the unit or space does not exist |
| 422 | no point starts at the address |

## Examples

```bash
curl -s http://127.0.0.1:9102/api/devices/1/holding/10/value
```

```json
{"value":21.5}
```

## See also

- [Set a decoded value](api-set-value.md)
- [Read values](api-read-values.md)
