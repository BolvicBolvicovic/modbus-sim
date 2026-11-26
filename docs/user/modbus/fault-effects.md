# Faults on the wire

What each fault kind does to the response and the connection. The fault options (`percent`, `schedule`, limits) are in the config reference; this page describes the effect on the Modbus traffic.

Faults are a simulator feature for testing clients. A device may configure several kinds at once (at
most one definition per kind), and each kind fires from its `percent` chance or from its
non-repeating `schedule`. The canonical rules for schedules and limits are in the
[config reference](../config/faults/index.md).

Faults never change request processing: the request still runs, and its effects on the device stay.
Silent paths (broadcast, unknown unit, listen-only) never trigger a disconnect.

## Exception

A fired `exception` fault replaces the response that normal processing produced, including an
exception response, and does not undo the request's effects. A sent injected exception is counted
like a real one. A suppressed injected exception only goes up the no-response counter (real
exceptions keep their accounting even when dropped). Busy and NAK counts use the injected code,
not the byte after a `corrupt` flip.

## Sensor disconnect

A fired `sensor-disconnect` turns a successful FC 02 or FC 04 read into exception 04. Other function
codes and error responses are unchanged, the connection stays open, and a fired generic `exception`
fault wins over it.

## Corrupt

`corrupt` has three modes:

| Mode | Effect |
|---|---|
| `pdu` (default) | flips `resp[1]` of the response, including an injected exception response: typically the byte count or the first data byte (XOR with `0xFF`, or the function code itself for a one-byte response) |
| `register` | flips a register data value in successful FC 03, 04 and 23 read responses and the echoed value of FC 06; other response shapes are unchanged |
| `checksum` | corrupts the emitted RTU CRC or ASCII LRC after framing, so the receiver sees a bad frame |

With `pdu` and `register` the transport computes the CRC (RTU), the LRC (ASCII) or the MBAP length
(TCP) over the altered PDU, so the receiver sees a structurally valid frame carrying wrong data. It
is never rejected for a checksum error and does not count in the FC 08 `0x000C` error counter.
MBAP has no checksum: `checksum` is rejected at load time in `tcp` and `udp`.

## Drop and disconnect

A `drop` fault suppresses the response. A `disconnect` fault implies a drop and closes the
connection; it is valid in `tcp`, `rtu-tcp` and `ascii-tcp` only. The serial modes, `udp`,
`ascii-udp` and `rtu-udp` reject it at load time.

## Delay

A configured delay is capped at 60000 ms. The response is held until the delay has passed without
blocking other clients. In the UDP modes delayed responses wait in a fixed 16-slot table, and a
response that finds the table full is dropped with a `WARN`.

## Restart

A fired `restart` leaves the current response intact, even when it is dropped or delayed. After the
response is built it resets configured points, ramp timelines and machines, and re-arms fault
schedules from the restart time (the `restart` kind keeps its own schedule progress).

It does not clear the protocol diagnostic counters, event logs, files, FIFOs, listen-only mode or the
random number seed.

## State machines

Fault overlays of a state machine follow the same transport restrictions as device faults: serial
modes reject `disconnect` overlays too, and a `checksum` overlay is rejected in `tcp` mode even if its
state is never active.

## See also

- [Exception codes](exceptions.md)
- [Diagnostics sub-functions](diagnostics-subfunctions.md)
- [Config reference: faults](../config/faults/index.md)
