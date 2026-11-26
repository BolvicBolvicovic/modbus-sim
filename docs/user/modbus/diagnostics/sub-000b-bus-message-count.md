# Return Bus Message Count (0x000B)

Diagnostics sub-function 0x000B returns how many requests the device has received.

## Syntax

```text
Request:   08 | 000B | 0000
Response:  08 | 000B | count (2)
```

## Valid in

- all transports

## Behaviour

- Returns the per-device message counter, including the counter-read request itself.
- It goes up once for every request PDU delivered to the device, including exceptions and requests
  ignored in listen-only mode, and wraps at 65536.
- [Return Slave Message Count (0x000E)](sub-000e-slave-message-count.md) returns the same value.
- Reset by [0x0001](sub-0001-restart-communications.md) and [0x000A](sub-000a-clear-counters.md).

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 000B 0000
response:  08 000B 0001
```

```python
print(client.diag_read_bus_message_count().message)
```

## See also

- [Diagnostics (0x08)](../fc08-diagnostics.md)
