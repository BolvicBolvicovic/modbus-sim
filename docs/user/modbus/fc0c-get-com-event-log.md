# Get Com Event Log (0x0C)

Function code 0x0C (decimal 12) returns the status, the event counter, the message counter and a list of configured event bytes.

## Syntax

```text
Request:   0C
Response:  0C | byte count (1) | status (2) | event count (2) | message count (2) | events (0-64)
Exception: 8C | code (1)
```

```scheme
(com-status N)
(com-event N)        ; repeatable, up to 64
```

## Default

`(com-status 0)`, no `com-event`: an empty log.

## Valid in

- all transports
- every device

## Behaviour

- `byte count` is `6 + number of event bytes`. With no events it is 6 and the response is 8 bytes.
- `status` comes from `(com-status N)`, the same setting as [Get Com Event Counter (0x0B)](fc0b-get-com-event-counter.md).
- `event count` is the same per-device counter as in 0x0B: successful completions only, not
  exception responses, poll commands or 0x0B/0x0C requests.
- `message count` goes up by one for every request PDU that reaches the device, including requests
  that end in an exception and this one. Both counters are returned after that increment and wrap
  to 0 after 65535.
- The events are the bytes of the repeated `(com-event N)` entries (0-255 each, at most 64), in
  configuration order. They are reported verbatim: mogbus does not interpret the serial-line event
  bits.
- The log starts with the configured bytes and is cleared only by Restart Communications with data
  `0xFF00` ([sub-function 0x0001](diagnostics/sub-0001-restart-communications.md)); `0x0000` leaves it.
- Requests for an unknown unit do not touch any device counter. A request with extra data is rejected.

## Errors

| Code | When |
|---|---|
| 03 | the request has data after the function code |

## Examples

With `(com-event 128) (com-event 255)`:

```text
request:   0C
response:  0C 08 0000 0009 000D 80 FF
```

```python
print(client.diag_get_comm_event_log().events)
```

## See also

- [Get Com Event Counter (0x0B)](fc0b-get-com-event-counter.md)
- [Diagnostics (0x08)](fc08-diagnostics.md)
