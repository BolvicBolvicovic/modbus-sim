# Force Listen Only Mode (0x0004)

Diagnostics sub-function 0x0004 puts the device in listen-only mode: it stops answering until it is restarted.

## Syntax

```text
Request:   08 | 0004 | 0000
Response:  none
```

## Valid in

- all transports

## Behaviour

- The device enters listen-only mode and sends no response to this request.
- Until a Restart Communications request ([0x0001](sub-0001-restart-communications.md), data `0x0000`
  or `0xFF00`), every other request to that device is ignored before PDU processing. The message
  counters (`0x000B`, `0x000E`) still go up for each monitored request; the event counter, the device
  state and the responses do not change.
- The no-response counter (`0x000F`) goes up for each ignored request, including this one. Bad RTU
  frames still go up the communication error counter (`0x000C`).
- Listen-only is per device and is not cleared by faults or by a `restart` fault.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 0004 0000
response:  (none)
```

## See also

- [Restart Communications Option (0x0001)](sub-0001-restart-communications.md)
- [Return Slave No Response Count (0x000F)](sub-000f-slave-no-response-count.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
