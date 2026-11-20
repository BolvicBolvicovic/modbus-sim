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
| 12 (0x0C) | Get Com Event Log | - | status + event/message counts + 0-64 events |
| 15 (0x0F) | Write Multiple Coils | coils | 1968 bits |
| 16 (0x10) | Write Multiple Registers | holding registers | 123 registers |
| 17 (0x11) | Report Server ID | - | byte count + server ID + run indicator |
| 20 (0x14) | Read File Record | file records | 1..N sub-requests (by PDU size) |
| 21 (0x15) | Write File Record | file records | 1..N sub-requests (by PDU size) |
| 22 (0x16) | Mask Write Register | holding registers | 1 register |
| 23 (0x17) | Read/Write Multiple Registers | holding registers | read 1-125, write 1-121 |

Read Exception Status (FC 07) takes no request data (PDU is only the function code) and returns a single status byte configured per device (`exception-status`, default 0). A request with any extra data is rejected with exception 03 (illegal data value).

Diagnostics (FC 08) takes a 2-byte subfunction and a 2-byte data field: request PDU `[0x08, sub-hi, sub-lo, data-hi, data-lo]` (5 bytes). This TCP simulator implements a minimal, stateless subset with no serial-line diagnostics counters/state: subfunction `0x0000` (Return Query Data) echoes the same 5-byte PDU back to the client. A request with any other length or shape is rejected with exception 03 (illegal data value). Any diagnostics subfunction other than `0x0000` is not supported and is rejected with exception 01 (illegal function).

Get Com Event Counter (FC 11 / 0x0B) is a serial-diagnostics function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x0B`, 1 byte). A normal response is `[FC 0x0B][Status Hi][Status Lo][Event Count Hi][Event Count Lo]` (5 bytes). In this simulator the status is always `0x0000` (device ready); busy/error status `0xFFFF` is not modeled. Each device keeps a 16-bit communication event counter that is incremented once for every Modbus request PDU processed for that device (including requests that produce an exception response and including the FC 11 request itself). The response returns the counter value after that increment, and the counter wraps to 0 after 65535. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value). (Note: exception code 0x0B in this project means Gateway Target Device Failed to Respond; it is unrelated to FC 11.)

Get Com Event Log (FC 12 / 0x0C) is also a serial-diagnostics function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x0C`, 1 byte). A normal response is `[FC 0x0C][Byte Count][Status Hi][Status Lo][Event Count Hi][Event Count Lo][Message Count Hi][Message Count Lo][Events...]` where Byte Count is the number of following bytes (`6 + number of event bytes`) and Status/Event Count/Message Count are 16-bit fields. In this simulator the status is always `0x0000` (device ready); the Event Count is the same per-device communication event counter returned by FC 11; the Message Count is a per-device 16-bit counter of Modbus request PDUs processed for the device. Both counters are incremented once for every handled request PDU (including exception-producing requests and the FC 12 request itself), the response returns their values after that increment, and each wraps to 0 after 65535. No serial-line events are modeled, so the Events field is always empty (0 bytes) and Byte Count is always `6`, producing an 8-byte PDU. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value).

Report Server ID (FC 17 / 0x11) is a serial-diagnostics identification function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x11`, 1 byte). A normal response is `[FC 0x11][Byte Count][Server ID][Run Indicator Status]` (4 bytes). Byte Count is the number of following bytes and is always `2` in this simulator (Server ID + Run Indicator Status), with no device-specific Additional Data. `Server ID` and `Run Indicator Status` are configured per device (`server-id`, 0-255, default `0`; `run-indicator`, 0-255, default `0xFF`). By Modbus convention Run Indicator Status `0xFF` means ON/run and `0x00` means OFF; other values are accepted by config and reported as-is. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value).

Read File Record (FC 20 / 0x14) reads 16-bit register records from per-device configured file storage (separate from the coil/discrete/input/holding tables). The request PDU is `[0x14][Byte Count][Sub-Req...]*` where Byte Count is `0x07-0xF5`, equals the total length of all sub-requests, and is a multiple of 7. Each sub-request is 7 bytes: `[Reference Type (1)=0x06][File Number (2)][Record Number (2, 0x0000-0x270F)][Record Length (2, in registers)]`. The normal response PDU is `[0x14][Resp Data Length][Sub-Resp...]*` where Resp Data Length equals the total length of all sub-responses. Each sub-response is `[File Resp Length (1)=1 + 2*Record Length][Reference Type (1)=0x06][Record Data (2*Record Length bytes)]`. Multiple sub-requests are supported in one request and the response preserves their order.

In this simulator file records are explicit config data (see [config-reference.md](config-reference.md)): only configured `(file/file, record)` ranges exist and are readable (defaults to none). A sub-request reads a contiguous range of `Record Length` registers starting at `Record Number` in `File Number`; every register in that range must be defined by configuration, otherwise the request is unavailable. Record contents are 16-bit register values as configured (or later written by FC 21) and are returned unchanged. File numbers 0-65535 may be configured; record numbers are limited to 0-9999 (`0x270F`) by the Modbus spec.

A request is rejected with exception 03 (illegal data value) if its shape or values are invalid: wrong PDU/request byte count or non-multiple-of-7 sub-request framing, any reference type other than `0x06`, any record number above `0x270F`, any record length of 0, or if the computed response would not fit in a Modbus PDU. It is rejected with exception 02 (illegal data address) if any requested file number or any record in any requested range is not configured/available. Checks are performed before producing a normal response (shape/range/capacity, then availability), so an error for any sub-request produces an exception and no partial success data.

Write File Record (FC 21 / 0x15) writes 16-bit register records in the same per-device configured file storage. The request PDU is `[0x15][Byte Count][Sub-Req...]*` where Byte Count is `0x09-0xFB` and equals the total length of all sub-requests. Each sub-request is `7 + 2*RecordLength` bytes: `[Reference Type (1)=0x06][File Number (2)][Record Number (2, 0x0000-0x270F)][Record Length (2, in registers, >= 1)][Record Data (2*RecordLength bytes)]` (1..N sub-requests). The normal response echoes the request PDU on success: `[0x15][Byte Count][Sub-Req...]*`.

Writable scope is coherent with FC 20 and config-backed storage: a sub-request may write only a contiguous range that is fully defined by configuration (defaults to none). Writes update the in-memory record values for that device and are readable back via FC 20; the config file is not changed and values are lost on restart. File numbers 0-65535 and record numbers 0-9999 (`0x270F`) follow the same limits as FC 20. File/record creation is not supported: an undefined file or any undefined/unwritable record in the range is unavailable.

All-or-nothing semantics apply across the whole request: all sub-requests are validated for shape/values/framing, record ranges, and availability/writability before any change is applied. A request is rejected with exception 03 (illegal data value) if Byte Count or request length/framing is invalid (`0x09-0xFB`, does not match the total encoded sub-request lengths, or a sub-request is truncated/misaligned), any reference type is not `0x06`, any record number is above `0x270F` or a range exceeds `0x270F`, or any record length is 0. It is rejected with exception 02 (illegal data address) if any requested file number or any record in any requested range is not configured/available. On any exception no file records are modified. FC 21 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

Mask Write Register (FC 22 / 0x16) modifies one holding register using an AND mask and an OR mask. The request PDU is `[0x16][Ref Addr Hi][Ref Addr Lo][And_Mask Hi][And_Mask Lo][Or_Mask Hi][Or_Mask Lo]` (7 bytes). The normal response echoes the request PDU on success. The referenced holding register is read and updated as `Result = (Current Contents AND And_Mask) OR (Or_Mask AND (NOT And_Mask))`; any 16-bit mask values are accepted.

Target scope and semantics are the same as a single holding-register write (FC 06): the reference address must be a configured/defined holding register address. A request to an undefined/out-of-table or otherwise unavailable holding-register address is rejected with exception 02 (illegal data address). The current value is taken from request-time device state (after normal tick processing), and applying the mask write follows existing write behavior, including freezing the owning point as static if it was a ramp. The request must be exactly 7 bytes; any other length or shape is rejected with exception 03 (illegal data value). FC 22 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

Read/Write Multiple Registers (FC 23 / 0x17) performs a write to holding registers followed by a read from holding registers in a single transaction. The request PDU is `[0x17][Read Starting Address Hi][Read Starting Address Lo][Quantity to Read Hi][Quantity to Read Lo][Write Starting Address Hi][Write Starting Address Lo][Quantity to Write Hi][Quantity to Write Lo][Write Byte Count][Write Registers Value...]` (10 + Write Byte Count bytes), where Quantity to Read is 1-125 (`0x0001-0x007D`), Quantity to Write is 1-121 (`0x0001-0x0079`), and Write Byte Count is `2 * Quantity to Write`. The normal response PDU is `[0x17][Byte Count][Read Registers Value...]` where Byte Count is `2 * Quantity to Read`.

Both the read range and the write range target holding registers only and must be fully defined/configured addresses (coherent with existing holding-register read/write semantics). Addresses are not created. The write operation is performed before the read operation, so the read result reflects post-write values, including when the read and write ranges overlap. Writes follow existing holding-register write behavior (e.g. freezing a ramp point to static).

The request is all-or-nothing: shape/values/lengths and availability of both ranges are validated before applying any write. A request is rejected with exception 03 (illegal data value) if the request is too short or has the wrong length for the declared byte count, Quantity to Read or Quantity to Write is out of range, or Write Byte Count is not `2 * Quantity to Write`. It is rejected with exception 02 (illegal data address) if any address in the read range or the write range is undefined/unavailable per existing semantics. On any exception no holding registers are modified. FC 23 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

Any other function code gets exception 01. Not supported yet: 24, 43.

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
Only coils (05, 15) and holding registers (06, 16, 22, 23) can be written: Modbus has no function code
that writes input registers or discrete inputs. A write to an address that exists only as an
input register or discrete input is exception 02, because the target table has no such point.
Read/Write Multiple Registers (FC 23, 0x17) is also all-or-nothing: availability of both the read and
write holding-register ranges is validated before applying the write, so an unavailable/malformed
request modifies no holding registers.
Write File Record (FC 21, 0x15) is also all-or-nothing: every sub-request is validated (shape and
configured availability/writability) before applying any change, so an unavailable or malformed
request modifies no file records. FC 21 can update only configured file-record ranges (see above).

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
