# Modbus support

## Transport

### Modbus TCP

A 7-byte MBAP header (transaction id, protocol id 0, length, unit id) followed by the PDU.
Clients may keep a connection open and send several requests; they are answered one after another
in order. A client that sends a header with a protocol id other than 0, or an impossible length,
is disconnected.

### Modbus RTU

Modbus RTU is served over a bidirectional byte stream selected with `(server (mode rtu-pty))` (a new
Linux PTY whose slave path is printed at startup) or `(server (mode rtu-device) (path "PATH"))` (an
existing serial-like device). The same PDU handlers and device model are used as for TCP; only
framing/transport differ.

Frame format: `[addr][PDU...][CRC lo][CRC hi]` (2-byte Modbus CRC-16, low byte first). A valid
frame contains 1 address byte, 1..253 PDU bytes, and 2 CRC bytes. Device-mode paths are opened with
`O_NONBLOCK` so startup does not hang on a blocking open (for example, a FIFO with no writer);
`modbus-sim` keeps the fd non-blocking for the poll loop. When the fd is a tty, `modbus-sim`
configures raw mode (`CS8`, `CLOCAL|CREAD`, no flow control) with the selected parity and stop bits,
and preserves the existing baud rate. The default is 8N1 (no parity, 1 stop bit); config
`(server (parity even))` / `(server (stop-bits 2))` select `none|even|odd|mark|space` parity and 1 or
2 stop bits. `mark` and `space` require `CMSPAR` support on the host. These settings apply only to
RTU byte-stream ttys; non-tty fds skip termios setup.

Frame boundaries use Modbus RTU inter-frame timing when a baud rate is known: t3.5 is 1.750 ms for
baud rates above 19200, otherwise `ceil(3.5 * 11 / baud * 1,000,000)` microseconds. The baud can be
set explicitly with `(server (baud N))`; baud values must be standard termios rates. For tty
devices, when no explicit baud is configured, `modbus-sim` reads the tty's current output baud and
derives t3.5 from it. `(server (frame-gap-ms N))` remains an explicit inter-frame silence override
and takes precedence over baud-derived timing.

For byte streams with no UART character timing (PTY/pipe) and no known baud, `modbus-sim` falls back
to `frame-gap-ms` if configured, otherwise the default 20 ms. In that fallback mode the boundary is
still a practical approximation because read() chunk boundaries do not expose true UART character
timing. When baud is known and no explicit `frame-gap-ms` override is set, a read() gap >= t3.5 is
treated as an inter-frame boundary.

Optional strict t1.5: for RTU byte streams only, `(server (strict-t15 on))` enables a best-effort
intra-frame t1.5 discard heuristic in baud-derived auto mode. A read() gap between t1.5 and t3.5
inside an incomplete frame discards that partial frame. It is opt-in because read() boundaries do
not reflect true UART character timing and can split valid frames on PTYs, real serial ports, and
other byte streams. It is never applied to RTU-over-TCP.

No response is produced for:

- **Bad CRC**: frames whose CRC does not match are silently ignored.
- **Unknown unit**: a valid frame addressed to a unit id with no configured device is ignored.
- **Broadcast (unit 0)**: ignored by default. With `(server (broadcast-execute on))`, a broadcast
  request is executed on every configured device (normal request handling: tick, counters, writes)
  and no response frame is sent. Fault delays/drops are not applied because there is no response to
  send.

### Modbus ASCII

Modbus ASCII is served over a bidirectional byte stream (Linux PTY or an existing serial-like
device path) via `(server (mode ascii-pty))` or `(server (mode ascii-device) (path "PATH"))`. It is a
separate transport/framing choice: it uses the same PDU handlers and device model as TCP/RTU, but it
is not Modbus TCP (MBAP) and there is no ASCII-over-TCP mode. ASCII accepts only `mode`, `seed`,
`baud` and (in device mode) `path` in `(server ...)`.

Frame format on the wire is `:` + uppercase hex digits for `[addr][PDU...][LRC]` + `CR LF`.
`addr` is one byte, the PDU is 1..253 bytes, and the LRC is one byte:
`LRC = (u8)(0 - sum(addr through PDU))`, excluding the LRC itself. A valid frame therefore has an
even number of hex digits. Responses use uppercase hex; received frames accept upper- or lowercase
hex. The maximum encoded frame is `1 + 2 + 2*253 + 2 + 2 = 513` bytes.

Framing is delimiter-based: a frame is complete as soon as `CR LF` arrives. There is no t3.5, t1.5,
`frame-gap-ms`, or `strict-t15` behavior for ASCII; complete frames are dispatched immediately,
including several complete frames delivered in one `read()`. A bounded RX buffer accumulates
partial frames across reads.

No response is produced, and no PDU is dispatched, for:

- **Malformed framing**: missing leading `:`, missing/wrong `CR LF`, odd hex count, invalid hex
  digit, overlong frame, or a lone `CR`/`LF` inside a frame.
- **Bad LRC**: the decoded LRC does not match the sum of address and PDU bytes.
- **Unknown unit**: no configured device has that unit id.
- **Broadcast (unit 0)**: intentionally ignored, as in RTU.
- **Fault drop**: the configured fault decision suppresses the response.

Delay faults still hold the ASCII response until the configured delay has elapsed, without blocking
other work in the poll loop. Unit ids must be 1-247 in ASCII modes, as in RTU; `--check` with an ASCII
mode applies the same check. `(server (baud N))` may set the serial line/PTY speed but never affects
ASCII framing or timing. When the fd is a tty, `modbus-sim` configures raw 8N1 (`CS8`, no parity, 1
stop bit, no flow control), **not** the traditional Modbus ASCII 7E1 settings. Each config selects
exactly one mode; TCP listen and RTU timing entries are rejected in ASCII modes.

### Modbus RTU over TCP

`(server (mode rtu-tcp))` serves Modbus RTU frames on a TCP listen socket. This is a separate
transport/framing choice from Modbus TCP (MBAP) and from RTU byte streams: `tcp` mode still uses
MBAP framing, RTU byte-stream modes still use byte-stream framing, and `rtu-tcp` uses RTU frames
(`[addr][PDU][CRC lo hi]`) carried over TCP connections.

The same PDU handlers, device model, unit-id routing, fault injection, and RTU CRC validation are
used. Bad CRC and unknown unit produce no response, matching RTU behavior. Broadcast (unit 0) is
ignored by default; `(server (broadcast-execute on))` executes it on every configured device with no
response, exactly as for RTU byte streams. `port` and `bind` select the listen socket, and
`idle-timeout-ms` closes idle connections. `baud` and `frame-gap-ms` select RTU inter-frame timing:
explicit `frame-gap-ms` takes precedence; otherwise baud-derived t3.5 is used when baud is known;
otherwise the 20 ms default fallback is used. t1.5 discard is never applied to RTU-over-TCP, because
TCP read() boundaries do not reflect UART character timing and can split valid pipelined requests.
Serial parity and stop bits do not apply either (no tty is configured), and their presence in the
config is rejected in `rtu-tcp` mode.

RTU-over-TCP is not Modbus TCP and is not a transparent gateway/proxy implementation; it is an
explicit listen mode selected in `(server ...)`.

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
| 08 | Diagnostics | - | public subfunctions 0x0000-0x0004, 0x000A-0x0012, 0x0014 |
| 11 (0x0B) | Get Com Event Counter | - | configurable status + event count |
| 12 (0x0C) | Get Com Event Log | - | configurable status + event/message counts + 0-64 configured event bytes |
| 15 (0x0F) | Write Multiple Coils | coils | 1968 bits |
| 16 (0x10) | Write Multiple Registers | holding registers | 123 registers |
| 17 (0x11) | Report Server ID | - | byte count + server ID + run indicator + 0-249 bytes additional data |
| 20 (0x14) | Read File Record | file records | 1..N sub-requests (by PDU size) |
| 21 (0x15) | Write File Record | file records | 1..N sub-requests (by PDU size) |
| 22 (0x16) | Mask Write Register | holding registers | 1 register |
| 23 (0x17) | Read/Write Multiple Registers | holding registers | read 1-125, write 1-121 |
| 24 (0x18) | Read FIFO Queue | FIFO queues | 0-31 register values |
| 43 (0x2B) | Read Device Identification (MEI 14 or legacy MEI 13) | device identification | 1-253 bytes total response |

Read Exception Status (FC 07) takes no request data (PDU is only the function code) and returns a single status byte configured per device (`exception-status`, default 0). A request with any extra data is rejected with exception 03 (illegal data value).

Diagnostics (FC 08) takes a 2-byte subfunction and an N x 2-byte data field: request PDU `[0x08, sub-hi, sub-lo, data...]`. Although the Modbus spec defines FC 08 as serial-line only, this simulator exposes it through TCP, RTU, and RTU-over-TCP. All subfunctions below are per-device and persist only while the simulator runs (no config fields). A request shorter than 3 bytes is rejected with exception 03 (illegal data value); an unknown subfunction in a longer request is rejected with exception 01 (illegal function), including vendor-specific subfunctions `0x0013` and `0x0015`. Every supported subfunction except `0x0000` requires exactly 5 bytes; other lengths are rejected with exception 03. For those subfunctions the data word is reserved and must be `0x0000` (exception 03 otherwise), except `0x0001` (accepts `0x0000` or `0xFF00`) and `0x0003` (`CHAR 00`: the delimiter is the high byte and the low byte must be 0). Counter responses are 5 bytes and fit the normal Modbus PDU buffer. Subfunctions `0x0004` and a Restart Communications request received while listen-only produce no response at all: the TCP/RTU transports suppress the response frame/MBAP message entirely.

| Subfunction | Name | Behavior |
|---|---|---|
| `0x0000` | Return Query Data | Echoes the request PDU unchanged, as before. Requires at least 5 bytes and an odd total PDU length (an even-length data field); even total lengths get exception 03. |
| `0x0001` | Restart Communications Option | Data must be `0x0000` or `0xFF00` (otherwise exception 03). Both values clear the FC 08 diagnostic counters and the diagnostic register and exit listen-only mode, then echo the request when not already listen-only. A restart received while listen-only is processed but answered with silence. `0xFF00` additionally clears the FC 11/12 communication event-log counter; `0x0000` leaves it unchanged. Restart Communications requests are not counted as successful completions in the event log. |
| `0x0002` | Read Diagnostic Register | Returns the 16-bit diagnostic register in the count field. Default `0x0000`; only bit `0x0020` (ASCII delimiter changed) is set by `0x0003`. Bit `0x0001` (character overrun) is never set. |
| `0x0003` | Change ASCII Delimiter | The high data byte is stored as the delimiter and the low byte must be `0` (otherwise exception 03), then the diagnostic register bit `0x0020` is set and the request is echoed. The stored value is diagnostic state only: ASCII transport uses `CR LF` framing and is unaffected. Default delimiter is LF (`0x0A`). |
| `0x0004` | Listen Only Mode | Enters per-device listen-only mode and returns no response. Until Restart Communications (`0x0000` or `0xFF00`) is received, all other requests to that device are ignored before PDU processing: message and event counters, device state, and responses do not change, but the no-response counter (`0x000F`) increments for each ignored request and bad RTU frames still increment the bus communication-error counter (`0x000C`). |
| `0x000A` | Clear Counters | Echoes the request and clears the FC 08 diagnostic counters and diagnostic register. It does not clear the FC 11/12 event-log counter and does not exit listen-only mode. Like every other request except restart, it is ignored while listen-only. |
| `0x000B` | Return Bus Message Count | Returns the per-device `message_count` (also used by FC 12), including the counter-read request itself. |
| `0x000C` | Return Bus Communication Error Count | Increments once for every device on an RTU bus when a bad-CRC/invalid RTU frame is discarded; TCP has no bus framing errors. Exception responses do not increment this counter. Starts at 0. |
| `0x000D` | Return Bus Exception Error Count | Increments for every exception response, including address errors. Starts at 0. |
| `0x000E` | Return Slave Message Count | Same per-device `message_count` as `0x000B`. |
| `0x000F` | Return Slave No Response Count | Increments whenever a response is suppressed by listen-only mode or a configured drop fault, including the listen-only and silent-restart requests themselves. Starts at 0. |
| `0x0010` | Return Slave NAK Count | Always 0; no NAK response is modeled. |
| `0x0011` | Return Slave Busy Count | Always 0; no busy exception is modeled. |
| `0x0012` | Return Bus Character Overrun Count | Always 0; there is no UART character stream in which an overrun can occur. |
| `0x0014` | Clear Overrun Counter and Flag | Requires data `0x0000`, then clears the bus character-overrun counter and diagnostic register bit `0x0001`, and echoes the request. Other counters and bits are unchanged. |

The shared `message_count` follows the FC 12 rule: it increments once for every handled request PDU, including exceptions and FC 08/11/12 requests, and wraps at 65536. Requests ignored in listen-only mode are not handled and do not increment it. FC 08 `0x0001` and `0x000A` reset it together with the other FC 08 diagnostic counters. The FC 11/12 `com_event_count` follows its own rule (successful completions only, excluding FC 11/12 and all Restart Communications requests) and is reset only by a `0xFF00` restart. All FC 08 diagnostic counters start at 0 and wrap at 65536; the no-response counter (`0x000F`) also increments when a configured drop fault suppresses a response.

Get Com Event Counter (FC 11 / 0x0B) is a serial-diagnostics function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x0B`, 1 byte). A normal response is `[FC 0x0B][Status Hi][Status Lo][Event Count Hi][Event Count Lo]` (5 bytes). The status is configured per device with `(com-status N)`, where `N` is `0` (device ready) or `65535` (busy); the default is `0`. Each device keeps a 16-bit communication event counter that is incremented once for each successful message completion. It is not incremented for exception responses, poll commands, or Fetch Event Counter commands (FC 11). The response returns the current counter value; the counter wraps to 0 after 65535. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value). (Note: exception code 0x0B in this project means Gateway Target Device Failed to Respond; it is unrelated to FC 11.)

Get Com Event Log (FC 12 / 0x0C) is also a serial-diagnostics function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x0C`, 1 byte). A normal response is `[FC 0x0C][Byte Count][Status Hi][Status Lo][Event Count Hi][Event Count Lo][Message Count Hi][Message Count Lo][Events...]` where Byte Count is the number of following bytes (`6 + number of event bytes`) and Status/Event Count/Message Count are 16-bit fields. The Status comes from the same per-device `(com-status N)` setting as FC 11 (default `0`); the Event Count is the same per-device communication event counter returned by FC 11 and follows the same increment rules (successful message completions only; not incremented for exception responses, poll commands, or FC 11/12 requests). The Message Count is a per-device 16-bit counter of Modbus request PDUs processed for the device; it is incremented once for every handled request PDU, including exception-producing requests and the FC 12 request itself. The response returns both counter values after the message-count increment and without counting the FC 12 itself as an event; each wraps to 0 after 65535. The Events field contains the per-device bytes configured with repeated `(com-event N)` entries (one byte 0-255 per entry, at most 64, default empty), in configuration order. The simulator reports those bytes verbatim and does not interpret serial-line event nibbles (it does not split an event byte into transmitter/receiver status or decode its meaning). The event log is static configuration data; no function in this build clears it. With no events configured the Events field is empty (0 bytes) and Byte Count is `6`, producing the same 8-byte response as before. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value).

Report Server ID (FC 17 / 0x11) is a serial-diagnostics identification function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x11`, 1 byte). A normal response is `[FC 0x11][Byte Count][Server ID][Run Indicator Status][Additional Data...]` (4 bytes plus Additional Data). Byte Count is the number of following bytes and equals `2 + Additional Data length`; it is `2` for the default empty Additional Data. `Server ID` and `Run Indicator Status` are configured per device (`server-id`, 0-255, default `0`; `run-indicator`, 0-255, default `0xFF`). By Modbus convention Run Indicator Status `0xFF` means ON/run and `0x00` means OFF; other values are accepted by config and reported as-is. Additional Data is configured per device with `(server-id-additional-data "HEXBYTES")`, an even-length hex string decoded to bytes (upper- and lowercase digits accepted, `""` means none). Odd-length strings, non-hex digits and values over 249 bytes are rejected at load and by `--check`; 249 bytes is the maximum that fits a 253-byte Modbus PDU after FC, Byte Count, Server ID and Run Indicator. With the default empty string the response is byte-for-byte the original 4-byte form. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value).

Read File Record (FC 20 / 0x14) reads 16-bit register records from per-device configured file storage (separate from the coil/discrete/input/holding tables). The request PDU is `[0x14][Byte Count][Sub-Req...]*` where Byte Count is `0x07-0xF5`, equals the total length of all sub-requests, and is a multiple of 7. Each sub-request is 7 bytes: `[Reference Type (1)=0x06][File Number (2, 0x0001-0xFFFF)][Record Number (2, 0x0000-0x270F)][Record Length (2, in registers)]`. The normal response PDU is `[0x14][Resp Data Length][Sub-Resp...]*` where Resp Data Length equals the total length of all sub-responses. Each sub-response is `[File Resp Length (1)=1 + 2*Record Length][Reference Type (1)=0x06][Record Data (2*Record Length bytes)]`. Multiple sub-requests are supported in one request and the response preserves their order.

In this simulator file records are explicit config data (see [config-reference.md](config-reference.md)): only configured `(file/file, record)` ranges exist and are readable (defaults to none). A sub-request reads a contiguous range of `Record Length` registers starting at `Record Number` in `File Number`; every register in that range must be defined by configuration, otherwise the request is unavailable. Record contents are 16-bit register values as configured (or later written by FC 21) and are returned unchanged. File numbers 1-65535 (`0x0001-0xFFFF`) may be configured; record numbers are limited to 0-9999 (`0x270F`) by the Modbus spec.

A request is rejected with exception 03 (illegal data value) if its shape or values are invalid: wrong PDU/request byte count or non-multiple-of-7 sub-request framing, any reference type other than `0x06`, any file number below `0x0001`, any record number above `0x270F`, any record length of 0, any requested record range extending beyond `0x270F`, or if the computed response would not fit in a Modbus PDU. It is rejected with exception 02 (illegal data address) if any requested file number or any record in any requested range is not configured/available. Checks are performed before producing a normal response (shape/range/capacity, then availability), so an error for any sub-request produces an exception and no partial success data.

Write File Record (FC 21 / 0x15) writes 16-bit register records in the same per-device configured file storage. The request PDU is `[0x15][Byte Count][Sub-Req...]*` where Byte Count is `0x09-0xFB` and equals the total length of all sub-requests. Each sub-request is `7 + 2*RecordLength` bytes: `[Reference Type (1)=0x06][File Number (2, 0x0001-0xFFFF)][Record Number (2, 0x0000-0x270F)][Record Length (2, in registers, >= 1)][Record Data (2*RecordLength bytes)]` (1..N sub-requests). The normal response echoes the request PDU on success: `[0x15][Byte Count][Sub-Req...]*`.

Writable scope is coherent with FC 20 and config-backed storage: a sub-request may write only a contiguous range that is fully defined by configuration (defaults to none). Writes update the in-memory record values for that device and are readable back via FC 20; the config file is not changed and values are lost on restart. File numbers 1-65535 (`0x0001-0xFFFF`) and record numbers 0-9999 (`0x270F`) follow the same limits as FC 20. File/record creation is not supported: an undefined file or any undefined/unwritable record in the range is unavailable.

All-or-nothing semantics apply across the whole request: all sub-requests are validated for shape/values/framing, record ranges, and availability/writability before any change is applied. A request is rejected with exception 03 (illegal data value) if Byte Count or request length/framing is invalid (`0x09-0xFB`, does not match the total encoded sub-request lengths, or a sub-request is truncated/misaligned), any reference type is not `0x06`, any file number below `0x0001`, any record number is above `0x270F` or a range exceeds `0x270F`, or any record length is 0. It is rejected with exception 02 (illegal data address) if any requested file number or any record in any requested range is not configured/available. On any exception no file records are modified. FC 21 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

Mask Write Register (FC 22 / 0x16) modifies one holding register using an AND mask and an OR mask. The request PDU is `[0x16][Ref Addr Hi][Ref Addr Lo][And_Mask Hi][And_Mask Lo][Or_Mask Hi][Or_Mask Lo]` (7 bytes). The normal response echoes the request PDU on success. The referenced holding register is read and updated as `Result = (Current Contents AND And_Mask) OR (Or_Mask AND (NOT And_Mask))`; any 16-bit mask values are accepted.

Target scope and semantics are the same as a single holding-register write (FC 06): the reference address must be a configured/defined holding register address. A request to an undefined/out-of-table or otherwise unavailable holding-register address is rejected with exception 02 (illegal data address). The current value is taken from request-time device state (after normal tick processing), and applying the mask write follows existing write behavior, including freezing the owning point as static if it was a ramp. The request must be exactly 7 bytes; any other length or shape is rejected with exception 03 (illegal data value). FC 22 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

Read/Write Multiple Registers (FC 23 / 0x17) performs a write to holding registers followed by a read from holding registers in a single transaction. The request PDU is `[0x17][Read Starting Address Hi][Read Starting Address Lo][Quantity to Read Hi][Quantity to Read Lo][Write Starting Address Hi][Write Starting Address Lo][Quantity to Write Hi][Quantity to Write Lo][Write Byte Count][Write Registers Value...]` (10 + Write Byte Count bytes), where Quantity to Read is 1-125 (`0x0001-0x007D`), Quantity to Write is 1-121 (`0x0001-0x0079`), and Write Byte Count is `2 * Quantity to Write`. The normal response PDU is `[0x17][Byte Count][Read Registers Value...]` where Byte Count is `2 * Quantity to Read`.

Both the read range and the write range target holding registers only and must be fully defined/configured addresses (coherent with existing holding-register read/write semantics). Addresses are not created. The write operation is performed before the read operation, so the read result reflects post-write values, including when the read and write ranges overlap. Writes follow existing holding-register write behavior (e.g. freezing a ramp point to static).

The request is all-or-nothing: shape/values/lengths and availability of both ranges are validated before applying any write. A request is rejected with exception 03 (illegal data value) if the request is too short or has the wrong length for the declared byte count, Quantity to Read or Quantity to Write is out of range, or Write Byte Count is not `2 * Quantity to Write`. It is rejected with exception 02 (illegal data address) if any address in the read range or the write range is undefined/unavailable per existing semantics. On any exception no holding registers are modified. FC 23 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

Read FIFO Queue (FC 24 / 0x18) reads a per-device configured FIFO/queue identified by a FIFO pointer address. The request PDU is `[0x18][FIFO Pointer Address Hi][FIFO Pointer Address Lo]` (3 bytes). The normal response PDU is `[0x18][Byte Count Hi][Byte Count Lo][FIFO Count Hi][FIFO Count Lo][FIFO Value Register...]*` where Byte Count is `2 + 2*FIFO Count` (the FIFO Count field plus the FIFO values), FIFO Count is `0-31`, and each FIFO value is a 16-bit register value.

FIFO queues are explicit, config-backed device storage (see [config-reference.md](config-reference.md)), separate from the coil/discrete/input/holding tables and file records. Only configured FIFO pointer addresses exist (defaults to none), so availability is deterministic and consistent with "only configured things exist": a request for an unconfigured/unavailable FIFO pointer address is rejected with exception 02 (illegal data address). A configured FIFO returns its `FIFO Count` register values in configured order (0..31 values). Reads are non-destructive: the queue contents and count are not modified by FC 24, so repeated reads of the same FIFO pointer return the same response (a simple, deterministic readback model for this simulator).

The request must be exactly 3 bytes; any other length or shape (short, extra data) is rejected with exception 03 (illegal data value). Config enforces at most 31 values per FIFO, so a configured FIFO response always fits within a Modbus PDU. FC 24 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs; requests for an unknown unit are handled at the server level and do not increment any device counter.

Read Device Identification (FC 43 / 0x2B) reads device identification objects. MEI type `0x0E` is the current assignment; `0x0D` is accepted as a legacy alias for compatibility with older editions that used 43/13 for Read Device Identification. The current Modbus Application Protocol assigns MEI `0x0D` to CiA/CANopen; this simulator does not implement CANopen. The request must be exactly `[0x2B][MEI][ReadDevId code][Object Id]` (4 bytes); the response echoes the request's MEI byte. The normal response PDU is `[0x2B][MEI][ReadDevId code][Conformity Level][More Follows][Next Object Id][Number Of Objects][Object list...]`. Each object in the list is `[Object Id][Object Length][Object Value...]`, where `Object Value` is ASCII bytes and `Object Length` is 1..244. The 244-byte maximum keeps any single object readable in one MEI response (7-byte header + 2-byte object header + 244 = 253 = `MODBUS_MAX_PDU`). FC 43 uses the same PDU and validation over TCP, RTU, and RTU-over-TCP; in RTU both MEI values have a known fixed 4-byte request PDU.

Supported ReadDevId codes are `01` basic, `02` regular, `03` extended, and `04` specific. Object ids `0x00` VendorName, `0x01` ProductCode, and `0x02` MajorMinorRevision are basic/mandatory and default to `modbus-sim`, `modbus-sim`, and `1.0`. Optional regular objects `0x03` VendorUrl, `0x04` ProductName, `0x05` ModelName, and `0x06` UserApplicationName are returned only when configured. Extended identification objects `0x80-0xFF` can be configured per device (at most 32; see [config-reference.md](config-reference.md)). Conformity Level is `0x81` with only mandatory objects, `0x82` if any regular object `0x03-0x06` is configured, and `0x83` if any extended object is configured (extended takes precedence). Number Of Objects is the count returned in the current response.

Read code `01` (basic) returns only objects `0x00-0x02`. Read code `02` (regular) streams objects `0x00-0x06` only and never includes extended objects. Read code `03` (extended) streams objects `0x00-0x06` first, then configured extended objects `0x80-0xFF` in ascending id order, skipping unconfigured optional objects. For stream access, an object id that is not a configured/present object (including `0x07-0x7F`) restarts at object 0; a configured extended object id starts there. Read code `04` (specific) returns one configured object; extended ids `0x80-0xFF` work when configured, while unconfigured extended ids and `0x07-0x7F` return exception 02. If a stream does not fit in a Modbus PDU, the response sets `More Follows=0xFF` and `Next Object Id` to the first object not included; the client resumes with another stream request using that id. A response with zero objects is exception 02. Unsupported MEI types return exception 01 (checked before request length, so an unsupported MEI returns 01 even when the request length is malformed); an invalid ReadDevId code, invalid request length, or other malformed request data returns exception 03. FC 43 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

Any other function code gets exception 01.

## Exceptions

| Code | Name | When |
|---|---|---|
| 01 | Illegal Function | the function code is not in the table above |
| 02 | Illegal Data Address | any address of the request is not defined in the config (or is above 65535) |
| 03 | Illegal Data Value | quantity is 0 or above the limit, the request has the wrong length or byte count, or a single-coil value is not `FF00` / `0000` |
| 04 | Slave Device Failure | only via `(fault (kind exception) (code 4))` |
| 05 | Acknowledge | only via `(fault (kind exception) (code 5))` |
| 06 | Slave Device Busy | only via `(fault (kind exception) (code 6))` |
| 07 | Negative Acknowledge | only via `(fault (kind exception) (code 7))` |
| 08 | Memory Parity Error | only via `(fault (kind exception) (code 8))` |
| 09 | reserved | never emitted and rejected by config |
| 0A (10) | Gateway Path Unavailable | only via `(fault (kind exception) (code 10))` |
| 0B (11) | Gateway Target Device Failed to Respond | no device has the requested unit id; also available via `(fault (kind exception) (code 11))` |

Checks run in this order: function, request shape and quantity (03), then addresses (02). Codes
04-08 and 0A are never produced by the simulator's own validation: they exist only for fault
injection (see [config-reference.md](config-reference.md)). An exception fault replaces the
response that normal processing produced, including an exception response, and does not undo the
request's effects. A corrupt fault instead flips the first payload byte of the normal response,
so clients may see a malformed value; the request is still processed normally.

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

In RTU and ASCII modes, unit ids must be in the range 1-247. Address 0 is Modbus broadcast and
248-255 are reserved by the Modbus serial specification; a device configured with one of these ids
cannot be addressed by a normal serial request. `modbus-sim` rejects these ids when starting in an
RTU or ASCII mode and when `--check` reads such a mode. RTU broadcast requests (address 0) are
received but are ignored unless `(server (broadcast-execute on))` is set; when enabled they execute
on all devices and still produce no response. ASCII broadcast frames are received but never executed.
MBAP/TCP unit 0 is unrelated: it is only a unit id and is never broadcast.

## Timing

- Values are computed from the clock when a request arrives. There is no background thread.
- Responses are sent as soon as they are ready, unless a delay fault holds them.
- One connection handles one request at a time; many connections are served concurrently
  (up to 32). More connections are refused.
- Optional `(server (idle-timeout-ms N))` closes a connection after `N` ms with no I/O
  progress (`0` disables it, the default). Connections waiting to send a delayed fault
  response are not considered idle. Idle closes are logged at `INFO`.

## Deviations from the spec

The previously tracked Modbus spec deviations have been resolved:

- **FC 11/12 event counter:** counts successful message completions only; not incremented for exception responses, poll commands, or Fetch Event Counter commands (FC 11/12). FC 12 message count still counts handled request PDUs, including exceptions.
- **FC 08 Diagnostics:** public subfunctions `0x0000-0x0004`, `0x000A-0x0012`, and `0x0014` are supported with per-device state. Return Query Data (`0x0000`) accepts and echoes an N x 2-byte data field, and malformed odd-length data is rejected as illegal data value (03). `0x0003` only stores the `CHAR 00` delimiter and sets a diagnostic-register bit; it does not change the ASCII transport's `CR LF` framing. Listen-only mode suppresses all requests before processing except Restart Communications, which is processed and also silent. RTU bad frames increment the bus communication-error counter for every device; exception responses do not. NAK, busy, and character-overrun counters are intentionally always 0.
- **File number 0:** rejected; config and PDU validation accept file numbers `0x0001-0xFFFF`.
- **FC 20/21 record range exceeding 0x270F:** rejected as illegal data value (03) for both FC 20 and FC 21.

Current intentional transport/behavior limits (not spec deviations in supported FC semantics) are documented in [Not supported (yet)](#not-supported-yet) and the RTU/ASCII/TCP transport sections above.

## Not supported (yet)

- True UART character-level t1.5 timing on byte-stream transports. Optional `(server (strict-t15 on))` enables a best-effort t1.5 discard heuristic at read() boundary granularity for RTU byte streams in baud-derived auto mode; it is never applied to RTU-over-TCP.
- ASCII-over-TCP: not implemented; ASCII is a serial byte-stream transport only
- IPv6
- TLS (Modbus/TCP Security)
