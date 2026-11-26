# Read FIFO Queue (0x18)

Function code 0x18 (decimal 24) reads the values of a configured FIFO queue.

## Syntax

```text
Request:   18 | FIFO pointer address (2)
Response:  18 | byte count (2) | FIFO count (2) | FIFO values (2 x FIFO count)
Exception: 98 | code (1)
```

```scheme
(fifo ADDRESS (values VALUE ...))
```

## Default

No FIFOs: every request fails with exception 02.

## Valid in

- FIFO queues
- all transports

## Behaviour

- `FIFO count` is 0 to 31 and `byte count` is `2 + 2 x FIFO count`. The values come back in the order
  they are configured.
- FIFO queues are config-backed storage, separate from the four tables and from file records. Only
  configured pointer addresses exist.
- Reads do not consume anything: repeating the request returns the same values.
- The request must be exactly 3 bytes long.
- Requests for an unknown unit are handled by the server and do not touch any device counter.

## Errors

| Code | When |
|---|---|
| 03 | the request is not 3 bytes long |
| 02 | the pointer address is not a configured FIFO |

## Examples

With `(fifo 100 (values #x1234 #x5678 42))`:

```text
request:   18 0064
response:  18 0008 0003 1234 5678 002A
```

## See also

- [Config reference: FIFO queues](../config/device/fifo.md)
- [Read File Record (0x14)](fc14-read-file-record.md)
