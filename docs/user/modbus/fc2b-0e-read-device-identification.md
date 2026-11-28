# Read Device Identification (0x2B / 0x0E)

Function code 0x2B (decimal 43) with MEI type 0x0E reads the device identification objects: vendor name, product code, revision and more.

## Syntax

```text
Request:   2B | 0E | read device ID code (1) | object ID (1)
Response:  2B | 0E | read device ID code (1) | conformity level (1) | more follows (1) | next object ID (1) | number of objects (1) | objects ...
             object: object ID (1) | length (1) | value (length bytes, ASCII)
Exception: AB | code (1)
```

```scheme
(vendor-name "Text")   (product-code "Text")   (major-minor-revision "Text")
(vendor-url "Text")    (product-name "Text")   (model-name "Text")
(user-application-name "Text")
(device-id-object ID "Text")                   ; ID 128-255, up to 32
```

## Default

`vendor-name "mogbus"`, `product-code "mogbus"`, `major-minor-revision "1.0"`; the other objects exist only when configured.

## Valid in

- all transports
- every device

## Behaviour

- The request is exactly 4 bytes. The response echoes MEI type `0x0E`.
- Read codes: `01` basic (objects 0x00-0x02), `02` regular (0x00-0x06, never extended objects),
  `03` extended (0x00-0x06, then the configured extended objects 0x80-0xFF in ascending order), `04`
  one specific object.
- Objects 0x03 to 0x06 are returned only when configured. Extended objects (0x80 to 0xFF, at most 32
  per device, 1 to 244 bytes of text each) come from `(device-id-object ID "Text")`. Every object
  value is at most 244 bytes, so one object always fits in one response.
- Conformity level is `0x81` with only the mandatory objects, `0x82` when any of objects 0x03-0x06 is
  configured, `0x83` when any extended object is configured (extended wins).
- Streaming (codes 01 to 03): an object ID that is not configured, including 0x07 to 0x7F, restarts
  at object 0; a configured extended ID starts there. When the objects do not fit in one PDU the
  response sets `more follows` to `0xFF` and `next object ID` to the first object left out; the client
  repeats the request with that ID.
- Code 04 returns exactly one configured object. An unconfigured or unknown ID is exception 02.
- `number of objects` is the count in this response.

## Errors

| Code | When |
|---|---|
| 01 | the MEI type is not `0x0D` or `0x0E`; this is checked before the length, so it wins over a malformed request |
| 03 | the request is not 4 bytes long, or the read device ID code is not 01-04 |
| 02 | the response would hold no objects (for example code 04 with an unconfigured ID) |

## Examples

Read the basic objects of a device with the default identification:

```text
request:   2B 0E 01 00
response:  2B 0E 01 81 00 00 03 00 06 6D6F67627573 01 06 6D6F67627573 02 03 312E30
```

The three objects are `mogbus`, `mogbus` and `1.0` as ASCII.

```python
print(client.read_device_information().information)
```

## See also

- [CANopen General Reference (0x2B / 0x0D)](fc2b-0d-canopen-general-reference.md)
- [Report Server ID (0x11)](fc11-report-server-id.md)
- [Config reference: device entries](../config/device/index.md)
- From C: @ref mogbus_client_read_device_identification "mogbus_client_read_device_identification()" ([the client](../library/library-client.md)); from Python: [the Python client](../python/python-client.md)
