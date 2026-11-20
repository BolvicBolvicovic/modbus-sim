# Modbus support

## Transport

Modbus TCP: a 7-byte MBAP header (transaction id, protocol id 0, length, unit id) followed by
the PDU. Clients may keep a connection open and send several requests; they are answered one
after another in order.

A client that sends a header with a protocol id other than 0, or an impossible length,
is disconnected.

## Function codes

| Code | Name | Table | Max per request |
|---|---|---|---|
| 01 | Read Coils | coils | 2000 bits |
| 02 | Read Discrete Inputs | discrete inputs | 2000 bits |
| 03 | Read Holding Registers | holding registers | 125 registers |
| 04 | Read Input Registers | input registers | 125 registers |
| 05 | Write Single Coil | coils | 1 |
| 06 | Write Single Register | holding registers | 1 |
| 07 | Read Exception Status | - | 1 byte (status) |
| 08 | Diagnostics | - | subfunction 0x0000 (echo) |
| 11 (0x0B) | Get Com Event Counter | - | status + event count |
| 15 (0x0F) | Write Multiple Coils | coils | 1968 bits |
| 16 (0x10) | Write Multiple Registers | holding registers | 123 registers |

Read Exception Status (FC 07) takes no request data (PDU is only the function code) and returns a single status byte configured per device (`exception-status`, default 0). A request with any extra data is rejected with exception 03 (illegal data value).

Diagnostics (FC 08) takes a 2-byte subfunction and a 2-byte data field: request PDU `[0x08, sub-hi, sub-lo, data-hi, data-lo]` (5 bytes). This TCP simulator implements a minimal, stateless subset with no serial-line diagnostics counters/state: subfunction `0x0000` (Return Query Data) echoes the same 5-byte PDU back to the client. A request with any other length or shape is rejected with exception 03 (illegal data value). Any diagnostics subfunction other than `0x0000` is not supported and is rejected with exception 01 (illegal function).

Get Com Event Counter (FC 11 / 0x0B) is a serial-diagnostics function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x0B`, 1 byte). A normal response is `[FC 0x0B][Status Hi][Status Lo][Event Count Hi][Event Count Lo]` (5 bytes). In this simulator the status is always `0x0000` (device ready); busy/error status `0xFFFF` is not modeled. Each device keeps a 16-bit communication event counter that is incremented once for every Modbus request PDU processed for that device (including requests that produce an exception response and including the FC 11 request itself). The response returns the counter value after that increment, and the counter wraps to 0 after 65535. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value). (Note: exception code 0x0B in this project means Gateway Target Device Failed to Respond; it is unrelated to FC 11.)

Any other function code gets exception 01. Not supported yet: 12, 17, 20-24, 43.

## Exceptions

| Code | Name | When |
|---|---|---|
| 01 | Illegal Function | the function code is not in the table above |
| 02 | Illegal Data Address | any address of the request is not defined in the config (or is above 65535) |
| 03 | Illegal Data Value | quantity is 0 or above the limit, the request has the wrong length or byte count, or a single-coil value is not `FF00` / `0000` |
| 0B (11) | Gateway Target Device Failed to Respond | no device has the requested unit id |

Checks run in this order: function, request shape and quantity (03), then addresses (02).

## Writes are all-or-nothing

If any address of a write is undefined, nothing is written and the answer is exception 02.
Only coils (05, 15) and holding registers (06, 16) can be written: Modbus has no function code
that writes input registers or discrete inputs. A write to an address that exists only as an
input register or discrete input is exception 02, because the target table has no such point.

## Unit ids

Each `(device ...)` answers to its own `(unit-id N)`. There is no "any unit id" mode: a client
using the wrong id gets exception 0B. If a client sends 255 or 0 by default, set the device's
`unit-id` to match.

## Timing

- Values are computed from the clock when a request arrives. There is no background thread.
- Responses are sent as soon as they are ready, unless a delay fault holds them.
- One connection handles one request at a time; many connections are served concurrently
  (up to 32). More connections are refused.

## Not supported (yet)

- Modbus RTU / serial and RTU-over-TCP (see the roadmap in [architecture.md](architecture.md))
- IPv6
- TLS (Modbus/TCP Security)
