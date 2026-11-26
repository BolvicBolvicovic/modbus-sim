# Change ASCII Input Delimiter (0x0003)

Diagnostics sub-function 0x0003 is recognized but always refused, so the end-of-message character of Modbus ASCII stays `CR LF`.

## Syntax

```text
Request:   08 | 0003 | delimiter (2)
Response:  88 | 03
```

## Valid in

- all transports

## Behaviour

- Always rejected with exception 03, whatever the data word or the request length.
- The end-of-message character is a per-port setting, not per-device state; ASCII framing is fixed at
  `CR LF`.

## Errors

| Code | When |
|---|---|
| 03 | always |

## Examples

```text
request:   08 0003 0A00
response:  88 03
```

## See also

- [Diagnostics (0x08)](../fc08-diagnostics.md)
- [Modbus ASCII](../../transports/transport-ascii.md)
