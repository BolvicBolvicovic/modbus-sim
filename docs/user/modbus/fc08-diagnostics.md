# Diagnostics (0x08)

Function code 0x08 runs a diagnostics sub-function: echo test, counters, listen-only mode and restart.

## Syntax

```text
Request:   08 | sub-function (2) | data (2 x N)
Response:  08 | sub-function (2) | data (2 x N)
Exception: 88 | code (1)
```

## Valid in

- all transports
- every device

## Behaviour

The Modbus specification defines FC 08 for serial lines only; mogbus exposes it on every transport.
The sub-functions are in [Diagnostics sub-functions](diagnostics-subfunctions.md). All state is per
device and lives only while the simulator runs; there are no config fields for it.

- A request shorter than 3 bytes is rejected with exception 03. An unknown sub-function in a longer
  request is rejected with exception 01, including the vendor sub-functions `0x0013` and `0x0015`.
- Every supported sub-function except `0x0000` takes exactly 5 bytes (function code, sub-function and
  one data word); other lengths are exception 03. The data word is reserved and must be `0x0000`
  (exception 03 otherwise), except for `0x0001`, which also accepts `0xFF00`. `0x0003` is recognized
  but always answered with exception 03.
- Counter responses are 5 bytes and fit the normal PDU buffer.
- Sub-function `0x0004` (Listen Only) and a Restart Communications request received while listen-only
  produce no response at all: the transport sends nothing. While listen-only, requests are still
  counted for the bus and slave message counts (`0x000B`, `0x000E`) but are not processed.

### How the counters work

- The shared message counter (`0x000B`, `0x000E`, and the message count of
  [Get Com Event Log (0x0C)](fc0c-get-com-event-log.md)) goes up once for every request PDU delivered to a
  device, including requests ignored in listen-only mode, exceptions and FC 08, 0x0B and 0x0C
  requests. It wraps at 65536.
- The communication event counter of 0x0B and 0x0C has its own rule: successful completions only,
  excluding 0x0B, 0x0C and all Restart Communications requests. It is reset by either Restart
  Communications data value but not by Clear Counters (`0x000A`).
- `0x0001` (both data values) and `0x000A` reset the message counter with the other FC 08 diagnostic
  counters.
- A sent injected exception replaces the accounting of the real result: it counts as one exception
  (busy or NAK according to the injected code) and not as a successful completion.
- All FC 08 counters start at 0 and wrap at 65536. The no-response counter (`0x000F`) also goes up
  when a drop or disconnect fault suppresses a response.

## Errors

| Code | When |
|---|---|
| 03 | the request is shorter than 3 bytes, has the wrong length for its sub-function, or the data word is not allowed |
| 01 | the sub-function is not supported |

## Examples

Return Query Data echoes the request:

```text
request:   08 0000 1234
response:  08 0000 1234
```

Return Bus Message Count:

```text
request:   08 000B 0000
response:  08 000B 0001
```

```python
print(client.diag_query_data(b"\x12\x34").message)
```

## See also

- [Diagnostics sub-functions](diagnostics-subfunctions.md)
- [Get Com Event Counter (0x0B)](fc0b-get-com-event-counter.md)
- [Faults on the wire](fault-effects.md)
