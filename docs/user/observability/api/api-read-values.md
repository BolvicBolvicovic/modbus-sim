# Read values

Reads raw register or bit values from a device.

## Syntax

```text
GET /api/devices/{unit}/{space}/{address}?count=N
```

## Default

`count` is 1

## Valid in

- `(control on)`
- all four tables

## Behaviour

- Returns `{"values":[...]}`: raw 16-bit values, or 0 and 1 for bits (`coil`, `discrete`).
- `count` is at most 125 registers or 2000 bits. Ramps and models are evaluated at the time of the
  request.
- Every address in the range must be configured.

## Errors

| Status | When |
|---|---|
| 400 | a bad number in the URL, or `count` out of range |
| 404 | the unit or space does not exist |
| 422 | an address of the range is not defined |

## Examples

```bash
curl -s "http://127.0.0.1:9102/api/devices/1/holding/0?count=1"
```

```json
{"values":[42]}
```

```json
{"error":"address range is not defined"}
```

## See also

- [Write values](api-write-values.md)
- [Read a decoded value](api-get-value.md)
