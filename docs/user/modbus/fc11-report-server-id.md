# Report Server ID (0x11)

Function code 0x11 (decimal 17) returns the server ID, a run indicator and optional additional data of the device.

## Syntax

```text
Request:   11
Response:  11 | byte count (1) | server ID (1) | run indicator (1) | additional data (0-249)
Exception: 91 | code (1)
```

```scheme
(server-id N)
(run-indicator N)
(server-id-additional-data "HEXBYTES")
```

## Default

`(server-id 0)`, `(run-indicator 255)` (`0xFF`), `(server-id-additional-data "")`

## Valid in

- all transports
- every device

## Behaviour

- `byte count` is `2 + length of additional data`: 2 with the default empty data, so the response is
  the 4-byte form `11 02 ID RUN`.
- `server-id` and `run-indicator` are 0 to 255. By convention `0xFF` means on (running) and `0x00`
  off; other values are accepted and reported as they are.
- `server-id-additional-data` is an even-length hex string (upper- or lowercase, `""` means none)
  of at most 249 bytes, the most that fits in a 253-byte PDU. Odd lengths, non-hex digits and longer
  values are rejected at load time and by `--check`.
- Requests for an unknown unit do not touch any device counter. A request with extra data is rejected.

## Errors

| Code | When |
|---|---|
| 03 | the request has data after the function code |

## Examples

With `(server-id 7)` and `(server-id-additional-data "AABB")`:

```text
request:   11
response:  11 04 07 FF AA BB
```

```python
print(client.report_slave_id().identifier)
```

## See also

- [Read Device Identification (0x2B / 0x0E)](fc2b-0e-read-device-identification.md)
- [Config reference: device entries](../config/device/index.md)
