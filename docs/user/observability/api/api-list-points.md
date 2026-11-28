# List points

Lists the configured points of one table of a device, in address order, one page at a time.

## Syntax

```text
GET /api/devices/{unit}/points?space={space}[&from={address}][&limit={n}]
```

## Default

`from` 0, `limit` 100

## Valid in

- `(control on)`

## Behaviour

- `space` is required: `coil`, `discrete`, `input` or `holding`.
- Returns `points`, one object per point whose first address is `from` or higher, at most `limit`
  of them (1 to 200), sorted by address, and `next`: the first address of the following page, or
  `null` when there are no more points. To list every point, repeat the request with `from` set to
  `next` until it is `null`. A page is never cut short silently: a device can have 8192 points, and
  the pages keep every response inside the endpoint's output limit. With notes (up to 127 bytes each) an
  entry can be several hundred bytes, so a page can end before `limit` points: always follow `next`.
- Each point has `address`, `registers` (how many addresses it spans: 1 to 4, or the string length),
  `type` (`u16`, `s16`, `u32`, `s32`, `float32`, `u64`, `s64`, `float64`, `string`, or `bit` in the
  coil and discrete tables), `kind` (`static`, `ramp`, `model`, `clock` or `machine`), `read_only`
  (`(access read-only)`), and `word_order` (`high-first` or `low-first`) and `byte_order` (`big` or
  `little`) to decode the raw registers of a read, and `note` (the point's `(note "...")`, JSON-escaped) only
  when it has one.
- `kind` is the current kind: a point written over Modbus or the API becomes `static`.
- No request body.

## Errors

| Status | When |
|---|---|
| 400 | `space` is missing or unknown, or `from` or `limit` is out of range |
| 404 | the control API is off, or the unit does not exist |
| 405 | the method is not GET |

## Examples

```bash
curl -s 'http://127.0.0.1:9102/api/devices/1/points?space=holding&limit=2'
```

```json
{"points":[{"address":0,"registers":1,"type":"u16","kind":"static","read_only":false,"word_order":"high-first","byte_order":"big"},{"address":10,"registers":2,"type":"float32","kind":"ramp","read_only":false,"word_order":"high-first","byte_order":"big"}],"next":null}
```

## See also

- [Runtime control API](control-api.md)
- [List devices](api-list-devices.md)
- [Read values](api-read-values.md)
