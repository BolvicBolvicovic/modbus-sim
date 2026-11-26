# Return Slave NAK Count (0x0010)

Diagnostics sub-function 0x0010 returns how many Negative Acknowledge exceptions the device sent.

## Syntax

```text
Request:   08 | 0010 | 0000
Response:  08 | 0010 | count (2)
```

## Valid in

- all transports

## Behaviour

- Goes up for each exception 07 (Negative Acknowledge) response, including injected ones with
  `(fault (kind exception) (code 7))`. Otherwise 0.
- The count follows the injected code, not the byte after a `corrupt` flip.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 0010 0000
response:  08 0010 0000
```

## See also

- [Return Slave Busy Count (0x0011)](sub-0011-slave-busy-count.md)
- [Exception codes](../exceptions.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
