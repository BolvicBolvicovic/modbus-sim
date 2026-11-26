# Clear Overrun Counter and Flag (0x0014)

Diagnostics sub-function 0x0014 clears the character overrun counter and its flag in the diagnostic register.

## Syntax

```text
Request:   08 | 0014 | 0000
Response:  08 | 0014 | 0000     an echo
```

## Valid in

- all transports

## Behaviour

- The data word must be `0x0000`. The bus character overrun counter and bit `0x0001` of the
  diagnostic register are cleared and the request is echoed. Other counters and bits are unchanged.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 0014 0000
response:  08 0014 0000
```

## See also

- [Return Bus Character Overrun Count (0x0012)](sub-0012-bus-character-overrun-count.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
