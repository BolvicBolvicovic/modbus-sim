# Return Slave Busy Count (0x0011)

Diagnostics sub-function 0x0011 returns how many Device Busy exceptions the device sent.

## Syntax

```text
Request:   08 | 0011 | 0000
Response:  08 | 0011 | count (2)
```

## Valid in

- all transports

## Behaviour

- Goes up for each exception 06 (Device Busy) response, including injected ones with
  `(fault (kind exception) (code 6))`. Otherwise 0.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 0011 0000
response:  08 0011 0000
```

## See also

- [Return Slave NAK Count (0x0010)](sub-0010-slave-nak-count.md)
- [Exception codes](../exceptions.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
