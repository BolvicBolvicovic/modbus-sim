# Return Bus Character Overrun Count (0x0012)

Diagnostics sub-function 0x0012 returns the bus character overrun count, which is always 0.

## Syntax

```text
Request:   08 | 0012 | 0000
Response:  08 | 0012 | 0000
```

## Valid in

- all transports

## Behaviour

- Always 0: there is no UART character stream in which an overrun can occur.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 0012 0000
response:  08 0012 0000
```

## See also

- [Clear Overrun Counter and Flag (0x0014)](sub-0014-clear-overrun.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
