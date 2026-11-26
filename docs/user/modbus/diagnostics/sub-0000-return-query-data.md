# Return Query Data (0x0000)

Diagnostics sub-function 0x0000 echoes the request: a loopback test of the connection.

## Syntax

```text
Request:   08 | 0000 | data (2 x N)
Response:  08 | 0000 | the same data
```

## Valid in

- all transports

## Behaviour

- Echoes the request PDU unchanged.
- The request is at least 5 bytes with an even-length data field (an odd total PDU length); an even
  total length is exception 03.
- `mogbus --health` uses this request to test a running server.

## Errors

| Code | When |
|---|---|
| 03 | the request is shorter than 5 bytes or has an even total length |

## Examples

```text
request:   08 0000 1234
response:  08 0000 1234
```

## See also

- [Diagnostics (0x08)](../fc08-diagnostics.md)
- [Health check](../../deployment/health-check.md)
