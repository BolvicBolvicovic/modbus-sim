# Modbus support

## Transport

### Modbus TCP

A 7-byte MBAP header (transaction id, protocol id 0, length, unit id) followed by the PDU.
Clients may keep a connection open and send several requests; they are answered one after another
in order. A client that sends a header with a protocol id other than 0, or an impossible length,
is disconnected. `port` and `bind` select the listen socket; `bind` accepts a plain IPv4 or IPv6
literal address and defaults to `"127.0.0.1"` (loopback, local clients only). Accepting remote
clients requires an explicit `(bind "0.0.0.0")` or `(bind "::")`. IPv6 listen sockets set
`IPV6_V6ONLY`, so `"0.0.0.0"` and `"::"` listen on separate address families and cannot both be
used: a config may contain at most one `(server ...)` block (defaults apply if omitted), there is
no dual-stack option, and scoped link-local literals such as `"fe80::1%eth0"` are rejected by the
config validator. These bind rules
apply to every network mode (`tcp`, `rtu-tcp`, `ascii-tcp`, `udp`, `rtu-udp`, `ascii-udp`). MBAP has no
checksum field, so `(fault (kind corrupt) (mode checksum))` is rejected in `tcp` and `udp` modes; the RTU and ASCII transports
carry checksums and accept it.

`(server (tls ...))` wraps the TCP connection in TLS; the bytes inside the tunnel are the same MBAP
frames and receive the same unit routing, fault handling, and PDU answers. TLS is an optional build
feature (`WITH_TLS=auto` enables it when OpenSSL is installed; `WITH_TLS=0` omits it) and is available
only for `mode tcp`; a no-TLS build rejects `(tls ...)` at load time with `built without TLS support`.
Plaintext TCP, UDP, RTU and ASCII transports work identically in both builds. TLS defaults to port 802
when `(port N)` is omitted, and requires a server certificate and private key. Client certificates
are required by default; the CA certificate is then required as well. TLS 1.2 is the minimum, and
anonymous/NULL ciphers are disabled. `--check` validates TLS files
and ciphers by creating the context, just as startup does. An accepted TLS connection
must complete its handshake within `(tls (handshake-timeout-ms N))` (1-86400000, default `10000`),
measured from accept and independent of `(idle-timeout-ms)`. Stalled handshakes are closed and logged
at `WARN` with the peer and timeout, so they cannot hold all 32 client slots; connections that
finished the handshake are unaffected. A client certificate's subject is logged at `INFO` after a
successful handshake so the authenticated peer can be audited; private keys and secrets are never
logged. A configured CA is also advertised to clients as the acceptable client CA list, and failure
to load that list fails context creation. Every TLS close (idle timeout, I/O error, EOF, or server
shutdown) makes one nonblocking best-effort attempt to send `close_notify` and never waits for the
peer, so a peer that is not reading cannot stall the server. An ungraceful peer drop reported with
an empty OpenSSL error queue is treated as EOF rather than logged as a TLS failure. See
[config-reference.md](config-reference.md#tls-modbus-tcp-over-tls) for the full option list.

#### Modbus/TCP Security coverage

TLS mode implements the transport-security part of the Modbus/TCP Security specification: the MBAP
exchange is wrapped in TLS 1.2 or 1.3, the server presents a certificate, mutual TLS can require a
client certificate signed by the configured CA, the CA is advertised in the TLS certificate request,
TLS versions and cipher suites are configurable, and the authenticated peer subject is logged. This
is not full Modbus Security role-based access control: certificate revocation (CRL/OCSP) is not
checked, Modbus Security x.509 role OIDs are not interpreted, and a trusted client certificate grants
access to every configured device and unit. Clients must also trust the server certificate out of
band; the advertised CA does not provision client trust stores.

### Modbus RTU

Modbus RTU is served over a bidirectional byte stream selected with `(server (mode rtu-pty))` (a new
Linux PTY whose slave path is printed at startup) or `(server (mode rtu-device) (path "PATH"))` (an
existing serial-like device). The same PDU handlers and device model are used as for TCP; only
framing/transport differ.

Frame format: `[addr][PDU...][CRC lo][CRC hi]` (2-byte Modbus CRC-16, low byte first). A valid
frame contains 1 address byte, 1..253 PDU bytes, and 2 CRC bytes. A `(fault (kind corrupt)
(mode checksum))` response is sent with a deliberately wrong CRC, so the receiver must reject it
as a bad frame; `pdu` and `register` corruption leave the recomputed CRC valid. Device-mode paths
are opened with `O_NONBLOCK` so startup does not hang on a blocking open (for example, a FIFO with
no writer); `mogbus` keeps the fd non-blocking for the poll loop. When the fd is a tty, `mogbus`
configures raw mode (`CS8`, `CLOCAL|CREAD`, no flow control) with the selected parity and stop bits,
and preserves the existing baud rate. The default is 8N1 (no parity, 1 stop bit); config
`(server (parity even))` / `(server (stop-bits 2))` select `none|even|odd|mark|space` parity and 1 or
2 stop bits. `mark` and `space` require `CMSPAR` support on the host. These settings apply only to
RTU byte-stream ttys; non-tty fds skip termios setup.

Frame boundaries use Modbus RTU inter-frame timing when a baud rate is known: t3.5 is 1.750 ms for
baud rates above 19200, otherwise `ceil(3.5 * 11 / baud * 1,000,000)` microseconds. The baud can be
set explicitly with `(server (baud N))`; baud values must be standard termios rates. For tty
devices, when no explicit baud is configured, `mogbus` reads the tty's current output baud and
derives t3.5 from it. `(server (frame-gap-ms N))` remains an explicit inter-frame silence override
and takes precedence over baud-derived timing.

For byte streams with no UART character timing (PTY/pipe) and no known baud, `mogbus` falls back
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
  and no response frame is sent. Fault delays/drops/disconnects are not applied because there is no
  response to send.

### Modbus ASCII

Modbus ASCII is served over a bidirectional byte stream (Linux PTY or an existing serial-like
device path) via `(server (mode ascii-pty))` or `(server (mode ascii-device) (path "PATH"))`, or over
a TCP listen socket via `(server (mode ascii-tcp))`. It is a separate transport/framing choice: it
uses the same PDU handlers and device model as TCP/RTU, but it is not Modbus TCP (MBAP). Serial ASCII
accepts `mode`, `seed`, `baud`, `data-bits`, `parity`, `stop-bits` and (in device mode) `path`;
`ascii-tcp` accepts only `mode`, `port`, `bind`, `seed`, and `idle-timeout-ms` in `(server ...)`.

Frame format on the wire is `:` + uppercase hex digits for `[addr][PDU...][LRC]` + `CR LF`.
`addr` is one byte, the PDU is 1..253 bytes, and the LRC is one byte:
`LRC = (u8)(0 - sum(addr through PDU))`, excluding the LRC itself. A valid frame therefore has an
even number of hex digits. Responses use uppercase hex; received frames accept upper- or lowercase
hex. The maximum encoded frame is `1 + 2 + 2*253 + 2 + 2 = 513` bytes. A `(fault (kind corrupt)
(mode checksum))` response carries a deliberately damaged LRC, so the receiver rejects it as a
bad frame; `pdu` and `register` corruption leave the recomputed LRC valid.

Framing is delimiter-based: a frame is complete as soon as `CR LF` arrives. There is no t3.5, t1.5,
`frame-gap-ms`, or `strict-t15` behavior for ASCII; complete frames are dispatched immediately,
including several complete frames delivered in one `read()`. A bounded RX buffer accumulates
partial frames across reads; a trailing `CR` waits for its `LF` in a later read. A second `:` before
the terminator starts a new frame: bytes up to that colon are discarded and the rest is rescanned,
so a truncated frame followed immediately by a valid request still delivers the request. Bytes
before a first colon that are only `CR`, `LF`, or space are ignored as inter-frame whitespace;
other garbage there is discarded. If a buffered frame receives no new byte for 1 s, the partial
frame is dropped so it cannot corrupt a later request.

Every discarded or undecodable ASCII frame (bad framing, invalid hex, bad LRC, or a timed-out
partial frame) increments the FC 08 bus communication error counter (`0x000C`) on every device.
Inter-frame `CR`, `LF`, and space are not errors. Successfully decoded frames do not, including
those answered with a Modbus exception.

No response is produced, and no PDU is dispatched, for:

- **Malformed framing**: missing leading `:`, missing/wrong `CR LF`, odd hex count, invalid hex
  digit, overlong frame, a `CR` followed by a non-`LF`, or a lone `LF` inside a frame.
- **Bad LRC**: the decoded LRC does not match the sum of address and PDU bytes.
- **Unknown unit**: no configured device has that unit id.
- **Broadcast (unit 0)**: intentionally ignored in every ASCII mode. `broadcast-execute` is rejected
  by config validation for ASCII, so a unit-0 frame is never executed on a device.
- **Fault drop or disconnect**: the configured fault decision suppresses the response; a
  disconnect also closes a TCP-like connection (never a serial fd).

Delay faults still hold the ASCII response until the configured delay has elapsed, without blocking
other work in the poll loop. Unit ids must be 1-247 in ASCII modes, as in RTU; `--check` with an ASCII
mode applies the same check. `(server (baud N))` may set the serial line/PTY speed but never affects
ASCII framing or timing. When the fd is a tty, `mogbus` configures raw 8N1 by default (8 data
bits, no parity, 1 stop bit, no flow control). The traditional Modbus ASCII 7E1 line format is opt-in:
`(server (data-bits 7) (parity even) (stop-bits 1))` sets `CS7`, even parity and 1 stop bit. Each
config selects exactly one mode; RTU timing and broadcast entries are rejected in ASCII modes, and
serial line settings are accepted only in the PTY/device modes.

### Modbus ASCII over TCP

`(server (mode ascii-tcp))` serves the same raw ASCII frames on TCP byte streams: `:` + uppercase
hex `[addr][PDU][LRC]` + `CR LF`, with no MBAP header and no TLS. `port` and `bind` select the listen
socket (defaults `1502` and `"127.0.0.1"`); `bind` accepts IPv4 or IPv6 literals, with IPv6 sockets
IPv6-only, as for Modbus TCP. Bind explicitly to `"0.0.0.0"` or `"::"` to accept remote clients.
Up to 32 clients are served by one poll loop, each with its own RX
buffer and delayed-response slot; `idle-timeout-ms` closes connections with no I/O progress.
Framing, resynchronization, error counting, unit routing, and fault behavior are identical to the
serial ASCII modes, except that `(fault (kind disconnect) ...)` is accepted here and closes the
client connection (serial modes reject it): complete frames are answered as soon as `CR LF` arrives, multiple frames per
read and split frames work, a trailing `CR` waits for the next `LF`, and a partial frame with no byte
for 1 s is dropped and counted as one FC 08 `0x000C` communication error per device. Broadcast
(unit 0) is silently ignored with no PDU processing. Serial options (`baud`, `frame-gap-ms`,
`strict-t15`, `data-bits`, `parity`, `stop-bits`, `path`) and `broadcast-execute` are rejected at load
time, including by `--check`.

### Modbus RTU over TCP

`(server (mode rtu-tcp))` serves Modbus RTU frames on a TCP listen socket. This is a separate
transport/framing choice from Modbus TCP (MBAP) and from RTU byte streams: `tcp` mode still uses
MBAP framing, RTU byte-stream modes still use byte-stream framing, and `rtu-tcp` uses RTU frames
(`[addr][PDU][CRC lo hi]`) carried over TCP connections.

The same PDU handlers, device model, unit-id routing, fault injection, and RTU CRC validation are
used. A `(fault (kind disconnect) ...)` decision closes the TCP connection after processing the
addressed frame; it is not applied to bad-CRC, unknown-unit, or broadcast frames, which stay silent
without disconnecting. Bad CRC and unknown unit produce no response, matching RTU behavior. Broadcast (unit 0) is
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

### Modbus UDP

`(server (mode udp))` serves Modbus TCP's MBAP ADU over UDP: one complete ADU (7-byte MBAP header
plus PDU) per datagram, no connection. Unit-id routing, PDU handlers, the unknown-unit exception
`0B` and fault injection are the same as in `tcp` mode; the reply is one datagram sent to the
request's source address, so several peers can talk to the same socket and each gets its own
answer. The MBAP header is echoed except for the length. A datagram is ignored (no response, DEBUG
log) when it is empty, longer than 260 bytes, has a protocol id other than 0, or its MBAP length
does not equal the datagram size minus 6. UDP gives no ordering or delivery guarantee, and the
simulator never retransmits; `drop` and `delay` faults model the loss and latency on top of it (a
delayed response waits in a 16-slot table and is dropped with a WARN if the table is full).
`disconnect` and `corrupt` mode `checksum` are rejected at load time. `port` (default 1502) and `bind`
(default `"127.0.0.1"`) select the socket; only `mode`, `port`, `bind` and `seed` are accepted. There
is no TLS (no DTLS) and no idle timeout. RTU and ASCII frames over UDP are the separate `rtu-udp`
and `ascii-udp` modes below.

### Modbus ASCII over UDP

`(server (mode ascii-udp))` carries Modbus ASCII frames (`:` + uppercase hex `[addr][PDU][LRC]` +
`CR LF`) one per datagram, no connection. Each datagram must be exactly one complete frame: there is
no stream, so no reassembly, no resynchronization on a second colon, no partial-frame timeout, and no
tolerated inter-frame whitespace. A datagram without CR LF, with bytes before the colon or after the
LF, with two frames, with an odd or invalid hex count or with a bad LRC is ignored (no response, never
reaches PDU dispatch) and counts one FC08 `0x000C` communication error on every device, as in
`ascii-tcp`. Empty datagrams and those longer than 515 bytes (the largest ASCII frame) are dropped
before decoding without counting. Lowercase hex is accepted on RX; replies are uppercase, one frame per
datagram to the request's source address. Unit 0 (broadcast) and unknown units get no response and
never execute; FC08 `0x0003` is rejected with exception 03. Faults behave as in `ascii-tcp`
(`corrupt` mode `checksum` damages the reply LRC; delayed responses use the 16-slot table), except
`disconnect`, which is rejected at load time. `port` (default 1502) and `bind` (default `"127.0.0.1"`)
select the socket; only `mode`, `port`, `bind` and `seed` are accepted. There is no TLS, idle timeout,
serial line configuration or `broadcast-execute`.

### Modbus RTU over UDP

`(server (mode rtu-udp))` carries Modbus RTU frames (`[addr][PDU][CRC lo hi]`, CRC-16/Modbus) one
per UDP datagram. The datagram boundary delimits the frame, so there is no t3.5/t1.5 timing, no
`baud`, `frame-gap-ms` or `strict-t15`. Request semantics match `rtu-tcp`: a bad CRC, a frame
shorter than 4 bytes and an unknown unit get no response; unit 0 is broadcast with no response
and, with `(broadcast-execute on)`, executes on every device. The datagram must be exactly one
frame: two concatenated frames, trailing bytes, an empty datagram or one longer than 256 bytes are
rejected whole (nothing executes). Each rejected datagram of 4-256 bytes counts as one FC08
`0x000C` communication error on every device, as a bad frame does on `rtu-tcp`. The reply (a
single RTU frame) goes to the request's source address; `delay` faults wait in the shared 16-slot
table (dropped with a WARN when full). `disconnect` is rejected at load time; `corrupt` modes
`pdu`, `register` and `checksum` all work (checksum flips the last CRC byte). Accepted options:
`mode`, `port` (default 1502), `bind` (default `"127.0.0.1"`), `seed`, `broadcast-execute`.

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
| 43 (0x2B) | Encapsulated Interface Transport: Read Device Identification (MEI 14) and CANopen General Reference (MEI 13) | device identification or CANopen object dictionary | 1-253 bytes total response |

Read Exception Status (FC 07) takes no request data (PDU is only the function code) and returns a single status byte configured per device (`exception-status`, default 0). A request with any extra data is rejected with exception 03 (illegal data value).

Diagnostics (FC 08) takes a 2-byte subfunction and an N x 2-byte data field: request PDU `[0x08, sub-hi, sub-lo, data...]`. Although the Modbus spec defines FC 08 as serial-line only, this simulator exposes it through TCP, RTU, ASCII, and RTU-over-TCP. All subfunctions below are per-device and persist only while the simulator runs (no config fields). A request shorter than 3 bytes is rejected with exception 03 (illegal data value); an unknown subfunction in a longer request is rejected with exception 01 (illegal function), including vendor-specific subfunctions `0x0013` and `0x0015`. Every supported subfunction except `0x0000` requires exactly 5 bytes; other lengths are rejected with exception 03. For those subfunctions the data word is reserved and must be `0x0000` (exception 03 otherwise), except `0x0001` (accepts `0x0000` or `0xFF00`). `0x0003` is recognized but always answered with exception 03. Counter responses are 5 bytes and fit the normal Modbus PDU buffer. Subfunctions `0x0004` and a Restart Communications request received while listen-only produce no response at all: the TCP/RTU transports suppress the response frame/MBAP message entirely. While listen-only, requests are still monitored for the bus/slave message count (`0x000B`/`0x000E`) but are not otherwise processed.

| Subfunction | Name | Behavior |
|---|---|---|
| `0x0000` | Return Query Data | Echoes the request PDU unchanged, as before. Requires at least 5 bytes and an odd total PDU length (an even-length data field); even total lengths get exception 03. |
| `0x0001` | Restart Communications Option | Data must be `0x0000` or `0xFF00` (otherwise exception 03). Both values clear all FC 08 diagnostic counters, the diagnostic register, and the FC 11/12 communication event counter, and exit listen-only mode; then the request is echoed when not already listen-only. `0xFF00` also clears the FC 12 communications event log (event bytes and log length); `0x0000` leaves the log intact. A restart received while listen-only is processed but answered with silence. Restart Communications requests are not counted as successful completions in the event log. |
| `0x0002` | Read Diagnostic Register | Returns the 16-bit diagnostic register in the count field. Default `0x0000`; no supported subfunction sets a bit in it, and bit `0x0001` (character overrun) is never set. |
| `0x0003` | Change ASCII Delimiter | Always rejected with exception 03 (illegal data value), whatever the data word or request length. The end-of-message character is a per-port setting, not per-device state; ASCII framing is fixed at `CR LF`. |
| `0x0004` | Listen Only Mode | Enters per-device listen-only mode and returns no response. Until Restart Communications (`0x0000` or `0xFF00`) is received, all other requests to that device are ignored before PDU processing: the message count (`0x000B`/`0x000E`) still increments for each monitored request, while the event counter, device state, and responses do not change. The no-response counter (`0x000F`) increments for each ignored request and bad RTU frames still increment the bus communication-error counter (`0x000C`). |
| `0x000A` | Clear Counters | Echoes the request and clears the FC 08 diagnostic counters and diagnostic register. It does not clear the FC 11/12 communication event counter or the FC 12 event log, and does not exit listen-only mode. Like every other request except restart, it is ignored while listen-only. |
| `0x000B` | Return Bus Message Count | Returns the per-device `message_count` (also used by FC 12), including the counter-read request itself. |
| `0x000C` | Return Bus Communication Error Count | Increments once for every device when a bad-CRC/invalid RTU frame or a malformed/bad-LRC/timed-out ASCII frame is discarded; TCP has no bus framing errors. Exception responses do not increment this counter. Starts at 0. |
| `0x000D` | Return Bus Exception Error Count | Increments for every exception response, including address errors and injected exception faults that are actually sent. Starts at 0. |
| `0x000E` | Return Slave Message Count | Same per-device `message_count` as `0x000B`. |
| `0x000F` | Return Slave No Response Count | Increments whenever a response is suppressed by listen-only mode, a configured drop fault, or a disconnect fault (which implies drop), including the listen-only and silent-restart requests themselves. Starts at 0. |
| `0x0010` | Return Slave NAK Count | Increments for exception 07 (Negative Acknowledge) responses, including injected ones; otherwise 0. |
| `0x0011` | Return Slave Busy Count | Increments for exception 06 (Device Busy) responses, including injected ones; otherwise 0. |
| `0x0012` | Return Bus Character Overrun Count | Always 0; there is no UART character stream in which an overrun can occur. |
| `0x0014` | Clear Overrun Counter and Flag | Requires data `0x0000`, then clears the bus character-overrun counter and diagnostic register bit `0x0001`, and echoes the request. Other counters and bits are unchanged. |

The shared `message_count` follows the FC 12 rule: it increments once for every request PDU delivered to a device, including requests ignored in listen-only mode, exceptions, and FC 08/11/12 requests, and wraps at 65536. FC 08 `0x0001` (both data values) and `0x000A` reset it together with the other FC 08 diagnostic counters. The FC 11/12 `com_event_count` follows its own rule (successful completions only, excluding FC 11/12 and all Restart Communications requests) and is reset by either Restart Communications data value but not by `0x000A`. A sent injected exception replaces the real result's accounting: it counts as one exception (with busy/NAK per the injected code) and not as a successful completion. All FC 08 diagnostic counters start at 0 and wrap at 65536; the no-response counter (`0x000F`) also increments when a configured drop or disconnect fault suppresses a response.

Get Com Event Counter (FC 11 / 0x0B) is a serial-diagnostics function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x0B`, 1 byte). A normal response is `[FC 0x0B][Status Hi][Status Lo][Event Count Hi][Event Count Lo]` (5 bytes). The status is configured per device with `(com-status N)`, where `N` is `0` (device ready) or `65535` (busy); the default is `0`. Each device keeps a 16-bit communication event counter that is incremented once for each successful message completion. It is not incremented for exception responses, poll commands, or Fetch Event Counter commands (FC 11). The response returns the current counter value; the counter wraps to 0 after 65535. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value). (Note: exception code 0x0B in this project means Gateway Target Device Failed to Respond; it is unrelated to FC 11.)

Get Com Event Log (FC 12 / 0x0C) is also a serial-diagnostics function carried as a normal PDU over Modbus TCP. The request is the function code only (`0x0C`, 1 byte). A normal response is `[FC 0x0C][Byte Count][Status Hi][Status Lo][Event Count Hi][Event Count Lo][Message Count Hi][Message Count Lo][Events...]` where Byte Count is the number of following bytes (`6 + number of event bytes`) and Status/Event Count/Message Count are 16-bit fields. The Status comes from the same per-device `(com-status N)` setting as FC 11 (default `0`); the Event Count is the same per-device communication event counter returned by FC 11 and follows the same increment rules (successful message completions only; not incremented for exception responses, poll commands, or FC 11/12 requests). The Message Count is a per-device 16-bit counter of Modbus request PDUs processed for the device; it is incremented once for every handled request PDU, including exception-producing requests and the FC 12 request itself. The response returns both counter values after the message-count increment and without counting the FC 12 itself as an event; each wraps to 0 after 65535. The Events field contains the per-device bytes configured with repeated `(com-event N)` entries (one byte 0-255 per entry, at most 64, default empty), in configuration order. The simulator reports those bytes verbatim and does not interpret serial-line event nibbles (it does not split an event byte into transmitter/receiver status or decode its meaning). The event log starts with the configured bytes and is cleared only by FC 08 Restart Communications Option with data `0xFF00`; a restart with data `0x0000` leaves it unchanged. With no events configured the Events field is empty (0 bytes) and Byte Count is `6`, producing the same 8-byte response as before. Requests for an unknown unit are handled at the server level and do not increment any device counter. A request with any extra data is rejected with exception 03 (illegal data value).

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

Read Device Identification (FC 43 / 0x2B) reads device identification objects. MEI type `0x0E` (14) is Read Device Identification; MEI `0x0D` (13) is CANopen General Reference (see below). The request must be exactly `[0x2B][0x0E][ReadDevId code][Object Id]` (4 bytes); the response echoes MEI `0x0E`. The normal response PDU is `[0x2B][0x0E][ReadDevId code][Conformity Level][More Follows][Next Object Id][Number Of Objects][Object list...]`. Each object in the list is `[Object Id][Object Length][Object Value...]`, where `Object Value` is ASCII bytes and `Object Length` is 1..244. The 244-byte maximum keeps any single object readable in one MEI response (7-byte header + 2-byte object header + 244 = 253 = `MODBUS_MAX_PDU`). FC 43 uses the same PDU and validation over TCP, RTU, and RTU-over-TCP. In RTU, MEI `0x0E` has a fixed 4-byte request PDU, while MEI `0x0D` is variable-length and framed from its count field.

Supported ReadDevId codes are `01` basic, `02` regular, `03` extended, and `04` specific. Object ids `0x00` VendorName, `0x01` ProductCode, and `0x02` MajorMinorRevision are basic/mandatory and default to `mogbus`, `mogbus`, and `1.0`. Optional regular objects `0x03` VendorUrl, `0x04` ProductName, `0x05` ModelName, and `0x06` UserApplicationName are returned only when configured. Extended identification objects `0x80-0xFF` can be configured per device (at most 32; see [config-reference.md](config-reference.md)). Conformity Level is `0x81` with only mandatory objects, `0x82` if any regular object `0x03-0x06` is configured, and `0x83` if any extended object is configured (extended takes precedence). Number Of Objects is the count returned in the current response.

Read code `01` (basic) returns only objects `0x00-0x02`. Read code `02` (regular) streams objects `0x00-0x06` only and never includes extended objects. Read code `03` (extended) streams objects `0x00-0x06` first, then configured extended objects `0x80-0xFF` in ascending id order, skipping unconfigured optional objects. For stream access, an object id that is not a configured/present object (including `0x07-0x7F`) restarts at object 0; a configured extended object id starts there. Read code `04` (specific) returns one configured object; extended ids `0x80-0xFF` work when configured, while unconfigured extended ids and `0x07-0x7F` return exception 02. If a stream does not fit in a Modbus PDU, the response sets `More Follows=0xFF` and `Next Object Id` to the first object not included; the client resumes with another stream request using that id. A response with zero objects is exception 02. Unsupported MEI types (anything other than `0x0D` and `0x0E`) return exception 01 (checked before request length, so an unsupported MEI returns 01 even when the request length is malformed); an invalid ReadDevId code, invalid request length, or other malformed request data returns exception 03. FC 43 participates in normal handled-request processing (diagnostic counters/tick/routing) like other supported FCs.

CANopen General Reference (FC 43 / MEI `0x0D`) supports SDO upload (read) and SDO download (write) against configured CANopen object dictionary entries. The request PDU is `[0x2B][0x0D][control][00][network-id?][data-type?][node][index-hi][index-lo][subindex][start-hi][start-lo][count-hi][count-lo][data...]`. Multi-byte address, start and count fields are big-endian. The protocol control byte uses CiA 309-2 bit positions: `0x01` selects write (download) instead of read (upload), `0x04` adds the optional network-id byte, which is echoed in the response, and `0x02` adds the optional data-type byte. Only default Unsigned8 data is implemented: if `0x02` is set, or if extend (`0x80`), counter (`0x20`) or extended-control (`0x40`) is set, the response is the AE extended exception below. The reserved byte must be `0` (otherwise exception 03). An upload request has no data and `count >= 1`; a download request carries exactly `count` data bytes and sets `count` to `0000` in its response. Normal responses echo the control byte, network-id byte, node, index, subindex and start address, with the upload response carrying `count` data bytes.

CANopen failures use the extended exception form `AB FF <len-hi> <len-lo> 0D <AE|CE> <payload>` (the length counts the MEI type, exception byte and payload). Unsupported options return `AE` with the supported-control byte `0x04 | (0x01 if write)` and length `0003`. CANopen service failures return `CE` with a 4-byte little-endian error code and length `0006`: `0xFFFF000B` object does not exist, `0xFFFF0011` invalid count/length (`count = 0` or a count/data-length mismatch), `0x06070012` requested length exceeds the configured object, `0xFFFF001E` response too large for a Modbus PDU, and `0xFFFF1003` service not supported. A truncated MEI 0x0D header still returns standard exception 03. Command addressing (`node = 0x7F`, `index = 0xFFFF`) is recognized but always returns `CE / 0xFFFF1003` because the simulator has no CAN gateway or virtual nodes. Only object-dictionary SDO access is implemented; PDO, NMT, LSS and extended-fragment services are not. Known CiA data-type values are used only for RTU length calculation (Unsigned8 `0x00`, Unsigned16 `0x01`, Unsigned24 `0x02`, Unsigned32 `0x03`, Integer16 `0x11`, Integer32 `0x13`, Real32 `0x20`, Real64 `0x21`, Visible string `0x50`, Domain `0x60`; unknown values default to one byte), so a request that selects encoded data can still be answered with AE.

### User-defined function codes

Any other function code gets exception 01, unless the device defines it with
`(function-code ...)` (see [config-reference.md](config-reference.md#function-code--user-defined-function-codes)).
A device can answer a code 1-127 that mogbus does not implement with a fixed reply, an echo of the
request data, an exception, live register or bit values like FC 01-04, or no response, selected by
the leading bytes of the request data; a request that no case matches gets exception 03. The Modbus
specification reserves 65-72 and 100-110 for user-defined codes; other unused codes are allowed too,
because real products use them. The 19 codes in the table above cannot be redefined.

A user-defined code is processed like a built-in one: the request counts in the FC 08/11/12
diagnostic counters and in [metrics](observability.md), exception answers are counted as exceptions,
listen-only mode suppresses the response, and faults apply (`delay`, `drop`, `exception`, `corrupt`,
`disconnect`, `restart`). `sensor-disconnect` affects only FC 02 and 04. Writes are not implied:
a user-defined code changes no device state, and `(registers ...)` only reads.

## Exceptions

| Code | Name | When |
|---|---|---|
| 01 | Illegal Function | the function code is not in the table above |
| 02 | Illegal Data Address | any address of the request is not defined in the config (or is above 65535), a write targets a machine-owned point such as a state register, or a write touches a point configured `(access read-only)` |
| 03 | Illegal Data Value | quantity is 0 or above the limit, the request has the wrong length or byte count, or a single-coil value is not `FF00` / `0000` |
| 04 | Slave Device Failure | FC 01/02/03/04/23 and user-defined `(registers ...)` reads covering a point whose `(degradation (fail-after-ms ...))` has passed; successful FC 02/04 reads via `(fault (kind sensor-disconnect))`; any response via `(fault (kind exception) (code 4))` |
| 05 | Acknowledge | only via `(fault (kind exception) (code 5))` |
| 06 | Slave Device Busy | only via `(fault (kind exception) (code 6))` |
| 07 | Negative Acknowledge | only via `(fault (kind exception) (code 7))` |
| 08 | Memory Parity Error | only via `(fault (kind exception) (code 8))` |
| 09 | reserved | never emitted and rejected by config |
| 0A (10) | Gateway Path Unavailable | only via `(fault (kind exception) (code 10))` |
| 0B (11) | Gateway Target Device Failed to Respond | no device has the requested unit id; also available via `(fault (kind exception) (code 11))` |

Checks run in this order: function, request shape and quantity (03), then addresses (02). Codes
05-08 and 0A are never produced by the simulator's own validation: they exist only for fault
injection (see [config-reference.md](config-reference.md#fault-)); 04 is also produced by a fired
`sensor-disconnect`. Fault injection is a simulator testing feature, not a Modbus protocol
feature: a device may configure several kinds at once (at most one definition per kind), and each
kind fires from its `percent` chance or its non-repeating `schedule`; schedule timing and limits
are canonical in [config-reference.md](config-reference.md#fault-).

State-machine fault overlays follow the same transport restrictions as device faults: serial modes
reject `disconnect` overlays too, and a checksum-corrupt overlay is rejected in `tcp` mode even if
its state is never active.

A fired exception fault replaces the response that normal processing produced, including an
exception response, and does not undo the request's effects; a sent injected exception is counted
like a real one, while a suppressed injected exception only increments the no-response counter
(real exceptions keep their accounting even when dropped). Busy/NAK counts use the injected code,
not the byte after a corrupt flip. A fired `sensor-disconnect` turns a successful FC 02 or FC 04
read into exception 04; other function codes and error responses are unchanged, the connection
stays open, and a fired generic `exception` code wins over it.

`corrupt` has three modes. `pdu` (the default) flips `resp[1]` of the response, including an
injected exception response; that is typically the byte count or the first data byte (XOR with
`0xFF`, or the function code itself for a one-byte response). `register` flips a register data
value in successful FC 03/04/23 read responses and the echoed value of FC 06, leaving other
response shapes unchanged. In both modes the transport computes CRC (RTU), LRC (ASCII), or the
MBAP length (TCP) over the altered PDU, so receivers see a structurally valid frame carrying wrong
data: it is never rejected for a CRC/LRC error and does not increment the FC08 `0x000C`
communication-error counters. `checksum` instead corrupts the emitted RTU CRC or ASCII LRC after
framing, so the receiver sees a bad frame; MBAP/TCP has no checksum and rejects this mode at load.

A drop fault suppresses the response, and a disconnect fault implies a drop and closes the
connection on `tcp`, `rtu-tcp` and `ascii-tcp` (serial modes, `udp`, `ascii-udp` and `rtu-udp` reject disconnect). A fired `restart`
leaves the current response intact, even when it is dropped or delayed, but resets configured
points, ramp timelines and machines after the response is built and re-arms fault schedules from
the restart time (the restart kind keeps its own schedule progress); it does not clear protocol
diagnostic counters, event logs, files, FIFOs, listen-only mode or the RNG seed.
A configured delay is capped at 60000 ms. Silent responses (listen-only and requests with no
response) are always suppressed without disconnecting. Faults do not change request processing: the
request still runs normally, and silent/broadcast/unknown-unit paths never trigger a disconnect.

## Writes are all-or-nothing

If any address of a write is undefined, nothing is written and the answer is exception 02.
Only coils (05, 15) and holding registers (06, 16, 22, 23) can be written: Modbus has no function code
that writes input registers or discrete inputs. A write to an address that exists only as an
input register or discrete input is exception 02, because the target table has no such point.
A machine-owned point, including a machine state register, is read-only to Modbus clients: a write
that touches any of its addresses fails all-or-nothing with exception 02 and leaves the value
unchanged. A coil or holding point configured `(access read-only)` is refused the same way (all-or-nothing, exception 02, counted as an exception); FC 22 and the write range of FC 23 are checked before anything is written or read. Successful writes run any matching machine `on-write` handlers in the same request after
the raw values are applied; those handlers do not fire transitions or advance the machine.
Read/Write Multiple Registers (FC 23, 0x17) is also all-or-nothing: availability of both the read and
write holding-register ranges is validated before applying the write, so an unavailable/malformed
request modifies no holding registers.
Write File Record (FC 21, 0x15) is also all-or-nothing: every sub-request is validated (shape and
configured availability/writability) before applying any change, so an unavailable or malformed
request modifies no file records. FC 21 can update only configured file-record ranges (see above).

## Unit ids

Each `(device ...)` must set `(unit-id N)` and answers only to that id. There is no "any unit id" mode: a client
using the wrong id gets exception 0B. If a client sends 255 or 0 by default, set the device's
`unit-id` to match.

In RTU and ASCII modes, unit ids must be in the range 1-247. Address 0 is Modbus broadcast and
248-255 are reserved by the Modbus serial specification; a device configured with one of these ids
cannot be addressed by a normal serial request. `mogbus` rejects these ids when starting in an
RTU or ASCII mode and when `--check` reads such a mode. RTU broadcast requests (address 0) are
received but are ignored unless `(server (broadcast-execute on))` is set; when enabled they execute
on all devices and still produce no response. ASCII broadcast frames are received but never executed,
and `broadcast-execute` is rejected at load time for ASCII modes.
MBAP/TCP unit 0 is unrelated: it is only a unit id and is never broadcast.

## Timing

- Values are computed from the clock when a request arrives. There is no background thread.
- Responses are sent as soon as they are ready, unless a delay fault holds them. A disconnect
  fault closes the client connection instead of sending the suppressed response.
- One connection handles one request at a time; many connections are served concurrently
  (up to 32). More connections are refused.
- Optional `(server (idle-timeout-ms N))` closes a connection after `N` ms with no I/O
  progress (`0` disables it, the default). Connections waiting to send a delayed fault
  response are not considered idle. Idle closes are logged at `INFO`.
- TLS handshakes have their own deadline from `(tls (handshake-timeout-ms N))`, default `10000`
  ms, independent of the idle timeout. Stalled handshakes are closed at `WARN`; this does not
  change request timing after the handshake completes.
- TLS closes send a `close_notify` before releasing the session. The wait is best-effort and
  bounded, so an unresponsive peer cannot delay shutdown indefinitely.

## Spec coverage at a glance

All standard public function codes are implemented at the FC level, written here as decimal FC with
hex in parentheses: FC 1 (0x01) through FC 8 (0x08), FC 11 (0x0B), FC 12 (0x0C), FC 15 (0x0F),
FC 16 (0x10), FC 17 (0x11), FC 20 (0x14) through FC 24 (0x18), and FC 43 (0x2B). FC 43 implements
both standardized MEI types: 0x0E (Read Device Identification) and 0x0D (CANopen General Reference,
limited to object-dictionary SDO upload/download). All other MEI types return exception 01, as do
reserved and vendor-defined FCs.

Normal validation emits exception codes 01, 02, 03, and 0B (and 04 for reads of failed degraded points), plus FC 43 / MEI 0x0D extended
exceptions (`AB FF ... 0D AE/CE ...`). Codes 05-08 and 0A are available only through the
simulator's repeatable `(fault (kind exception) (code N))` injection; 04 is also emitted by a
`sensor-disconnect` fault and by reads of a failed `(degradation ...)` point; 09 is reserved. The remaining fault kinds (delay, drop, corrupt,
disconnect, restart) are simulator-side behavior, not Modbus exceptions, and disconnect is allowed
only on TCP-like transports.

Notable remaining gaps include simplified serial-line diagnostics and config-backed data that is not
created dynamically or persisted, plus the limits in [Not supported (yet)](#not-supported-yet) and
[Transport](#transport): TLS is build-optional and only covers MBAP/TCP (not RTU-over-TCP,
ASCII-over-TCP or any UDP mode), and there is no true UART t1.5 timing (the RTU `strict-t15` option is only a
baud-derived-auto-mode approximation).

## Not supported (yet)

- True UART character-level t1.5 timing on byte-stream transports. Optional `(server (strict-t15 on))` enables a best-effort t1.5 discard heuristic at read() boundary granularity for RTU byte streams in baud-derived auto mode; it is never applied to RTU-over-TCP, and RTU-over-UDP has no inter-frame timing (one frame per datagram).
- TLS for RTU-over-TCP, ASCII-over-TCP, UDP, ASCII-over-UDP and RTU-over-UDP (no DTLS); `(server (tls ...))` is rejected outside `mode tcp`
- Certificate revocation checking (CRL/OCSP); a client certificate signed by the configured CA is accepted for the lifetime configured in the certificate
- Modbus Security role authorization from x.509 role OIDs; a valid client certificate is either required or optional, with no per-role access control
- Dual-stack IPv6 listening: IPv6 sockets are `IPV6_V6ONLY` and a config may contain at most one `(server ...)` block, so `"::"` and `"0.0.0.0"` cannot be served together; scoped link-local literals such as `fe80::1%eth0` are rejected
