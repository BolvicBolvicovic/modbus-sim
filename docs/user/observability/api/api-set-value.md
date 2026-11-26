# Set a decoded value

Sets the decoded value of a point, like a machine output: the host side of a sensor.

## Syntax

```text
PUT /api/devices/{unit}/{space}/{address}/value
{"value": N}
```

## Valid in

- `(control on)`
- all four tables

## Behaviour

- Sets the decoded number of the point whose first address is `address`.
- It works on every table. The point becomes host-owned: it stays at that value until the device
  restarts, ramps stop, and Modbus writes to it get exception 02.
- For bit points a nonzero value means on.

## Errors

| Status | When |
|---|---|
| 400 | malformed JSON, an unknown field or a missing `value` |
| 422 | no point starts at the address, or a machine drives the point |

## Examples

```bash
curl -s -X PUT -d '{"value":22.25}' http://127.0.0.1:9102/api/devices/1/holding/10/value
```

```json
{"ok":true}
```

## See also

- [Read a decoded value](api-get-value.md)
- [Write values](api-write-values.md)
