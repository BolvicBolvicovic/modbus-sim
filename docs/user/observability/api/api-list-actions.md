# List actions

Lists the [point actions](../../config/actions/index.md) of a device with the current value of each
param, in the device's action order, one page at a time.

## Syntax

```text
GET /api/devices/{unit}/actions[?from={index}][&limit={n}]
```

## Default

`from` 0, `limit` 50

## Valid in

- `(control on)`

## Behaviour

- Returns `actions`, one object per action instance starting at position `from` (actions are
  ordered by table, point address, then read before write), and `next`: the position of the following
  page, or `null` when there are no more. To list every action, repeat the request with `from` set to
  `next` until it is `null`. A page ends at `limit` (1 to 256) entries or earlier when the response
  buffer could not hold another worst-case entry, so a device with 256 actions and many params is
  never cut short silently.
- Each action has `space`, `address` (the point it is attached to), `event` (`read` or `write`),
  `name` (the `define-action` name, `""` for an inline body) and `params`: an array of objects with
  `name`, `type` (`number` or `bool`) and the current `value` (a number, or 0 or 1 for a `bool`).
- No request body.

## Errors

| Status | When |
|---|---|
| 400 | `from` or `limit` is out of range |
| 404 | the control API is off, or the unit does not exist |
| 405 | the method is not GET |

## Examples

```bash
curl -s 'http://127.0.0.1:9102/api/devices/1/actions'
```

```json
{"actions":[{"space":"holding","address":0,"event":"read","name":"counter","params":[{"name":"step","type":"number","value":5}]}],"next":null}
```

## See also

- [Runtime control API](control-api.md)
- [Set an action param](api-set-action-param.md)
- [param](../../config/actions/param.md)
