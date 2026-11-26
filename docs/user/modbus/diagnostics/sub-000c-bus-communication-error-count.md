# Return Bus Communication Error Count (0x000C)

Diagnostics sub-function 0x000C returns how many damaged frames were discarded.

## Syntax

```text
Request:   08 | 000C | 0000
Response:  08 | 000C | count (2)
```

## Valid in

- all transports

## Behaviour

- Goes up by one on every device each time a frame is discarded: a bad CRC or an invalid RTU frame,
  or a malformed, bad-LRC or timed-out ASCII frame.
- In `rtu-udp` and `ascii-udp` a rejected datagram counts once (RTU: 4 to 256 bytes; ASCII: any
  datagram that is not empty or oversized). MBAP (`tcp`, `udp`) has no bus framing errors.
- Exception responses do not count. The counter starts at 0.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 000C 0000
response:  08 000C 0000
```

## See also

- [Modbus RTU](../../transports/transport-rtu.md)
- [Modbus ASCII](../../transports/transport-ascii.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
