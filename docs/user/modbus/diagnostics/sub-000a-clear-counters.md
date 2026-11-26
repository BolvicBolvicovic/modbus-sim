# Clear Counters and Diagnostic Register (0x000A)

Diagnostics sub-function 0x000A resets the FC 08 counters and the diagnostic register.

## Syntax

```text
Request:   08 | 000A | 0000
Response:  08 | 000A | 0000     an echo
```

## Valid in

- all transports

## Behaviour

- Echoes the request and clears the FC 08 diagnostic counters and the diagnostic register.
- It does not clear the communication event counter or the event log of 0x0B and 0x0C, and it does
  not leave listen-only mode. Like every request except a restart, it is ignored while the device is
  listen-only.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 000A 0000
response:  08 000A 0000
```

## See also

- [Restart Communications Option (0x0001)](sub-0001-restart-communications.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
