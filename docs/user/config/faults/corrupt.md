# `(fault (kind corrupt))`

Corrupts the response so a client sees wrong data or a bad checksum.

## Syntax

```scheme
(fault (kind corrupt) [(mode pdu|register|checksum)] [(percent N)])
```

## Default

`mode pdu`, `percent 100`

## Valid in

- `(device ...)`
- every mode (`mode checksum`: not `tcp` and not `udp`)

## Behaviour

- `pdu` (the default) flips `resp[1]` (the byte count or first data byte; the function code for a
  one-byte response) with XOR `0xFF`, so the response length does not change.
- `register` changes a register data value in successful FC 03/04/23 read responses and the echoed
  value of FC 06; it leaves other response shapes unchanged.
- `checksum` deliberately corrupts the RTU CRC or ASCII LRC after the frame is built, so the receiver
  rejects the frame. MBAP/TCP has no checksum, so `checksum` is rejected in `tcp` mode and allowed in
  `rtu-pty`, `rtu-device`, `rtu-tcp`, `ascii-pty`, `ascii-device`, `ascii-tcp`, `ascii-udp` and
  `rtu-udp`; it is rejected in `udp` for the same reason.

`pdu` and `register` corruption keep the response length and leave the transport checksum valid: the
transport recomputes CRC (RTU), LRC (ASCII), or MBAP length (TCP), so receivers see a structurally
valid frame with wrong data, never a CRC/LRC error and no FC08 `0x000C` communication-error count.
`checksum` corruption is the opposite: the frame reaches the wire with a deliberately wrong CRC/LRC.
`pdu` or `checksum` corruption still applies to an injected [exception](exception.md).

A checksum-corrupt overlay is rejected in `tcp` mode even when the machine state it sits in is never
entered.

## Errors

- `corrupt mode checksum is not valid in tcp mode (MBAP frames have no checksum)`: MBAP/TCP has no checksum; use `pdu`/`register` or a serial/RTU transport.
- `mode is only valid for kind corrupt`: remove `(mode ...)` from other fault kinds.

## Examples

```scheme
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind corrupt) (mode register) (percent 50)))   ; half of the register reads carry a wrong value
```

## See also

- [exception](exception.md)
- [Modbus support](../../modbus/modbus-overview.md)
