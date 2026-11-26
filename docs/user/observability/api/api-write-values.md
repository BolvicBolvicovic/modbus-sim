# Write values

Writes raw register or bit values, the way a Modbus client would, but on any table.

## Syntax

```text
PUT /api/devices/{unit}/{space}/{address}
{"values":[N, ...]}
```

## Valid in

- `(control on)`
- all four tables

## Behaviour

- Works like a client write that also works on every table, including the read-only ones
  (`input`, `discrete`) and points configured `(access read-only)`: you play the physical side.
- A ramp or model that is written becomes static, and machine `on-write` handlers run.
- Bits take 0 or 1, registers 0 to 65535.
- All-or-nothing: the write is refused if an address is undefined, a point is driven by a machine,
  or a point was claimed with [`/value`](api-set-value.md).

## Errors

| Status | When |
|---|---|
| 400 | malformed JSON, an unknown field, a missing `values`, or a bad number |
| 413 | `Content-Length` over 1024 |
| 422 | an address is undefined, or a point is driven by a machine or by the host |

## Examples

```bash
curl -s -X PUT -d '{"values":[1234]}' http://127.0.0.1:9102/api/devices/1/holding/0
```

```json
{"ok":true}
```

```json
{"error":"write refused: undefined address or a point driven by a machine or the host"}
```

## See also

- [Read values](api-read-values.md)
- [Set a decoded value](api-set-value.md)
- [Writes are all-or-nothing](../../modbus/writes.md)
