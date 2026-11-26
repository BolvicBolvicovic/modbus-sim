# Return Slave Message Count (0x000E)

Diagnostics sub-function 0x000E returns how many requests were addressed to the device. It is the same value as sub-function 0x000B.

## Syntax

```text
Request:   08 | 000E | 0000
Response:  08 | 000E | count (2)
```

## Valid in

- all transports

## Behaviour

- The same per-device `message_count` as [Return Bus Message Count (0x000B)](sub-000b-bus-message-count.md).

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 000E 0000
response:  08 000E 0001
```

## See also

- [Diagnostics (0x08)](../fc08-diagnostics.md)
