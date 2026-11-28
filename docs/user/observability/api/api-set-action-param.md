# Set an action param

Changes one param of a [point action](../../config/actions/index.md); the new value is used from the
next run of the action.

## Syntax

```text
PUT /api/devices/{unit}/actions/{space}/{address}/{event}
{"name": "PARAM", "value": N}
```

## Valid in

- `(control on)`
- `{event}` is `read` or `write`

## Behaviour

- Selects the action by the point it is attached to (`space`, first `address`) and its event, and
  sets the param called `name`. List them with [List actions](api-list-actions.md).
- A `bool` param takes exactly 0 or 1; a `number` param takes any finite number.
- The value stays until the next change: a device restart resets variables but keeps params. Params
  are read-only to the action itself, so only this call (and the C and Python libraries) changes them.
- The change happens between Modbus requests, so the very next client request already uses it.

## Errors

| Status | When |
|---|---|
| 400 | malformed JSON, an unknown field, or `name` or `value` missing or of the wrong JSON type |
| 404 | no such action or param (also for an `{event}` other than `read` or `write`), or an unknown space |
| 422 | a `bool` param given a value other than 0 or 1 |

## Examples

```bash
curl -s -X PUT -d '{"name":"step","value":5}' http://127.0.0.1:9102/api/devices/1/actions/holding/0/read
```

```json
{"ok":true}
```

## See also

- [Runtime control API](control-api.md)
- [List actions](api-list-actions.md)
- [param](../../config/actions/param.md)
