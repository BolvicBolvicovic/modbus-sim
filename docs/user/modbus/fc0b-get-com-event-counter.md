# Get Com Event Counter (0x0B)

Function code 0x0B (decimal 11) returns a status word and the device's communication event counter.

## Syntax

```text
Request:   0B
Response:  0B | status (2) | event count (2)
Exception: 8B | code (1)
```

```scheme
(com-status N)
```

## Default

`(com-status 0)`

## Valid in

- all transports
- every device

## Behaviour

- `status` comes from `(com-status N)`: `0` (ready) or `65535` (busy).
- Every device keeps a 16-bit event counter. It goes up by one for each successful message
  completion and wraps to 0 after 65535. It does not count exception responses, poll commands or
  this function code (0x0B) and Get Com Event Log (0x0C) requests.
- The counter is reset by Restart Communications ([diagnostics sub-function 0x0001](diagnostics/sub-0001-restart-communications.md)) but not by Clear Counters (0x000A).
- Requests for an unknown unit are handled by the server and do not touch any device counter.
- A request with extra data is rejected.
- Exception code 0x0B in this project means Gateway Target Device Failed to Respond. It has nothing
  to do with this function code.

## Errors

| Code | When |
|---|---|
| 03 | the request has data after the function code |

## Examples

```text
request:   0B
response:  0B 0000 0009
```

```python
print(client.diag_get_comm_event_counter().count)
```

## See also

- [Get Com Event Log (0x0C)](fc0c-get-com-event-log.md)
- [Diagnostics (0x08)](fc08-diagnostics.md)
