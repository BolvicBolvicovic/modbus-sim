# Return Diagnostic Register (0x0002)

Diagnostics sub-function 0x0002 returns the 16-bit diagnostic register of the device.

## Syntax

```text
Request:   08 | 0002 | 0000
Response:  08 | 0002 | register (2)
```

## Valid in

- all transports

## Behaviour

- The register is 0 by default. No supported sub-function sets a bit in it, and bit `0x0001`
  (character overrun) is never set.
- The register is cleared by [Clear Counters (0x000A)](sub-000a-clear-counters.md) and by Restart
  Communications.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 0002 0000
response:  08 0002 0000
```

## See also

- [Diagnostics (0x08)](../fc08-diagnostics.md)
