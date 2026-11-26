# Restart Communications Option (0x0001)

Diagnostics sub-function 0x0001 clears the diagnostic counters and leaves listen-only mode.

## Syntax

```text
Request:   08 | 0001 | 0000 or FF00
Response:  08 | 0001 | the same data      (no response while listen-only)
```

## Valid in

- all transports

## Behaviour

- The data word must be `0x0000` or `0xFF00`, otherwise exception 03.
- Both values clear all FC 08 diagnostic counters, the diagnostic register and the communication
  event counter of 0x0B and 0x0C, and exit listen-only mode. The request is then echoed.
- `0xFF00` also clears the event log of [Get Com Event Log (0x0C)](../fc0c-get-com-event-log.md);
  `0x0000` leaves the log intact.
- A restart received while the device is listen-only is processed but answered with silence.
- Restart Communications requests do not count as successful completions in the event log.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or `0xFF00`, or the request is not 5 bytes long |

## Examples

```text
request:   08 0001 FF00
response:  08 0001 FF00
```

## See also

- [Listen Only Mode (0x0004)](sub-0004-listen-only-mode.md)
- [Clear Counters (0x000A)](sub-000a-clear-counters.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
