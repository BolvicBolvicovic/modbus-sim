# Return Slave No Response Count (0x000F)

Diagnostics sub-function 0x000F returns how many requests the device did not answer.

## Syntax

```text
Request:   08 | 000F | 0000
Response:  08 | 000F | count (2)
```

## Valid in

- all transports

## Behaviour

- Goes up whenever a response is suppressed by listen-only mode, a configured `drop` fault, or a
  `disconnect` fault (which implies a drop). The request that enters listen-only mode and a silent
  restart count too. The counter starts at 0.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 000F 0000
response:  08 000F 0000
```

## See also

- [Force Listen Only Mode (0x0004)](sub-0004-listen-only-mode.md)
- [Faults on the wire](../fault-effects.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
