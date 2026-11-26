# `(device (com-event ...))`

Adds one event byte to the log returned by Get Com Event Log (0x0C).

## Syntax

```scheme
(com-event N)
```

## Default

none (empty log)

## Valid in

- `(device ...)`

## Behaviour

Each `com-event` supplies one byte of the 0x0C Events field, in the order written. Values must be
0-255 and at most 64 entries are allowed; a 65th entry is rejected. 0x0C Byte Count is
`6 + number of event bytes`, so an empty log gives Byte Count `6` and an 8-byte PDU, and 64 events
give Byte Count `70`. The simulator reports the configured bytes verbatim and does not interpret
serial-line event nibbles (it does not split an event byte into transmitter/receiver status or decode
its meaning). The log is cleared by FC 08 Restart Communications Option with data `0xFF00`; data
`0x0000` leaves it unchanged.

## Examples

```scheme
(device (unit-id 1)
  (com-status 0)
  (com-event 128)
  (com-event 255)
  (holding 0))
```

## See also

- [com-status](com-status.md)
- [restart](../faults/restart.md)
