# Config reference

A config file describes the simulator: which transport and server options to use, and which devices
exist. Save it with any name, for example `my-device.scm`, and run `./mogbus my-device.scm`. To start from a register map you already have (CSV or pymodbus), see
[importing.md](importing.md).

## The format in one minute

The syntax is inspired by Scheme, but a config file is plain data: settings, not a program. These
rules cover all of it:

- Everything is in parentheses: `(name value)`. Every `(` needs a matching `)`.
- `;` starts a comment that runs to the end of the line.
- Text is in double quotes: `"Power meter"`.
- Numbers are written normally: `42`, `-5`, `0.5`, `230.4`.
- Hex numbers are written `#x1234` where examples use them.
- On/off can be written `on`, `off`, `yes`, `no`, `true`, `false`, `#t`, `#f`, `1` or `0`.
- Spaces, tabs and new lines are all the same. Indent however you like.
- Top-level forms and most entries can appear in any order. Exceptions: at most one `(server ...)`
  form; point address ranges cannot overlap; `(com-event ...)` entries are reported in the order
  written; and inside `(machine ...)`, `(initial ...)`, optional `(state-register ...)`, `(var ...)`,
  `(state ...)`, `(on-write ...)` and `(transition ...)` must be ordered as shown.

The file is only *read as data*. Nothing in it is run, so a typo can never do anything but
produce an error message.

## Overall shape

```scheme
(server ...)    ; optional, at most once

(device ...)    ; one or more
(device ...)
```

## `(server ...)`

`(mode NAME)` selects the transport. There are ten modes; device modes require `(path "PATH")`:

| Mode | Transport | `(path ...)` |
|---|---|---|
| `tcp` | Modbus TCP (MBAP) on a TCP listen socket, optionally wrapped in TLS | forbidden |
| `rtu-pty` | Modbus RTU on a new Linux PTY (slave path is printed at startup) | forbidden |
| `rtu-device` | Modbus RTU on an existing byte-stream device | required |
| `rtu-tcp` | Modbus RTU frames on a TCP listen socket (not MBAP) | forbidden |
| `ascii-pty` | Modbus ASCII on a new Linux PTY (slave path is printed at startup) | forbidden |
| `ascii-device` | Modbus ASCII on an existing byte-stream device | required |
| `ascii-tcp` | Modbus ASCII frames on a TCP listen socket (not MBAP) | forbidden |
| `udp` | Modbus UDP: one MBAP ADU per datagram on a UDP socket | forbidden |
| `ascii-udp` | Modbus ASCII: one `:`...CR LF frame per datagram on a UDP socket | forbidden |
| `rtu-udp` | Modbus RTU frames, one per datagram, on a UDP socket (not MBAP) | forbidden |

Only the entries listed for the selected mode are accepted. `mode` is allowed in every mode; an entry
that is present but not allowed is rejected at load time, even if its value would be the default
(for example `(stop-bits 1)` in `tcp` mode). `path` has its own required/forbidden checks.

`bind` must be a plain IPv4 or IPv6 literal: no hostnames and no zone/scoped link-local suffix such
as `"fe80::1%eth0"` (the validator uses `inet_pton()`, which does not accept `%iface`). IPv6 listen
sockets set `IPV6_V6ONLY`, so `"::"` serves IPv6 clients only. TCP listeners and UDP sockets default
to `"127.0.0.1"` (loopback); remote connections require an explicit `(bind "0.0.0.0")` for all IPv4
interfaces or `(bind "::")` for all IPv6 interfaces. A config file may contain at most one
`(server ...)` block; if it is omitted, the defaults in the table apply. There is no way to listen
on both families at once; there is no dual-stack option.

| Entry | Meaning | Default | Allowed in |
|---|---|---|---|
| `(port N)` | TCP or UDP port, 1-65535 | `1502`, or `802` when `(tls ...)` is present | `tcp`, `rtu-tcp`, `ascii-tcp`, `udp`, `ascii-udp`, `rtu-udp` |
| `(bind "ADDRESS")` | IPv4 or IPv6 literal address to listen on. `"0.0.0.0"` = all IPv4 interfaces (explicit opt-in for remote clients), `"127.0.0.1"` = this machine over IPv4 only (default), `"::"` = all IPv6 interfaces (explicit opt-in), `"::1"` = this machine over IPv6 only. IPv6 sockets are IPv6-only (no dual-stack), and scoped link-local literals such as `"fe80::1%eth0"` are rejected | `"127.0.0.1"` | `tcp`, `rtu-tcp`, `ascii-tcp`, `udp`, `ascii-udp`, `rtu-udp` |
| `(seed N)` | Seed for the random dice used by `percent`-based faults and by `(after-ms (random MIN MAX))` machine deadlines, 0-4294967295. Same seed gives the same sequences | `0` (a fixed built-in seed) | all modes |
| `(idle-timeout-ms N)` | Close a client connection after `N` milliseconds with no I/O progress, 0-86400000. `0` disables the timeout. A connection with a delayed response pending is not considered idle | `0` (disabled) | `tcp`, `rtu-tcp`, `ascii-tcp` |
| `(baud N)` | Standard termios serial baud rate (e.g. 9600, 19200, 38400, 115200). RTU derives t3.5 timing from it; in ASCII mode it only sets the serial line/PTY speed | `0` (unknown) | `rtu-pty`, `rtu-device`, `rtu-tcp`, `ascii-pty`, `ascii-device` |
| `(frame-gap-ms N)` | Explicit inter-frame silence override in milliseconds, 1-60000. Takes precedence over baud-derived t3.5 timing. If omitted and baud is unknown, RTU falls back to 20 ms | `0` (auto) | `rtu-pty`, `rtu-device`, `rtu-tcp` |
| `(strict-t15 on\|off)` | Opt-in best-effort t1.5 discard heuristic in baud-derived auto mode. Never applied to RTU-over-TCP | `off` | `rtu-pty`, `rtu-device` |
| `(broadcast-execute on\|off)` | When on, unit-0 (broadcast) requests execute on every configured device and produce no response. Never applies to MBAP/TCP or ASCII; supplying it in an ASCII mode is a load-time error | `off` | `rtu-pty`, `rtu-device`, `rtu-tcp`, `rtu-udp` |
| `(data-bits 7\|8)` | Number of tty data bits. ASCII defaults to 8; `(data-bits 7)` with `(parity even)` selects traditional 7E1 Modbus ASCII | `8` | `ascii-pty`, `ascii-device` |
| `(parity none\|even\|odd\|mark\|space)` | tty parity. `mark` and `space` require `CMSPAR` support | `none` | `rtu-pty`, `rtu-device`, `ascii-pty`, `ascii-device` |
| `(stop-bits 1\|2)` | Number of tty stop bits | `1` | `rtu-pty`, `rtu-device`, `ascii-pty`, `ascii-device` |
| `(path "PATH")` | Byte-stream device path; opened with `O_NONBLOCK` | none | `rtu-device`, `ascii-device` (required there) |
| `(http (port N) (bind "ADDR") (control on\|off))` | HTTP endpoint serving `/healthz` (200 while the server loop runs), `/readyz` (200 while serving, 503 before start and while stopping) and `/metrics` (Prometheus text format: requests by function code, responses by outcome, rejected frames, response latency histogram, connections, and per device requests, exceptions, fired faults, comm errors). Port default 9102, bind default `"127.0.0.1"`; in TCP modes the port must differ from the Modbus port. `(control on)` (default `off`) also serves the unauthenticated `/api/` control API that reads and writes registers and sets or clears faults at runtime, and logs a WARN at start; see [observability.md](observability.md) | off | all modes |
| `(tls ...)` | TLS settings for the TCP listener; see [TLS](#tls-modbus-tcp-over-tls). Rejected at load time in other modes, including `--check`, and in every mode of a no-TLS build | absent (plaintext) | `tcp` |

Examples below show alternatives; each `(server ...)` form is complete, and a config file may contain
only one.

```scheme
(server (mode tcp) (port 1502) (bind "127.0.0.1") (seed 1))
```

```scheme
(server (mode rtu-pty) (baud 9600) (frame-gap-ms 20)
        (parity even) (stop-bits 2) (strict-t15 on))
(server (mode rtu-device) (path "/dev/ttyUSB0") (baud 19200) (broadcast-execute on))
```

```scheme
(server (mode rtu-tcp) (port 1502) (bind "0.0.0.0") (baud 9600)
        (idle-timeout-ms 5000) (broadcast-execute on))
```

```scheme
(server (mode ascii-pty) (baud 9600))
(server (mode ascii-device) (path "/dev/ttyUSB1"))
(server (mode ascii-pty) (data-bits 7) (parity even) (stop-bits 1)) ; 7E1 line
```

```scheme
(server (mode ascii-tcp) (port 1502) (bind "127.0.0.1") (idle-timeout-ms 5000))
```

```scheme
; Modbus UDP: one MBAP ADU per datagram; replies go to the sender's address
(server (mode udp) (port 1502) (bind "127.0.0.1"))
```

`udp` accepts only `mode`, `port`, `bind` and `seed`; `tls`, `idle-timeout-ms` (there are no
connections), serial line options, RTU timing options and `broadcast-execute` are rejected at load
time. Unit ids 0-255 are allowed as in `tcp`. A datagram must hold exactly one MBAP ADU; others
(bad protocol id, length mismatch, empty, longer than 260 bytes) are ignored.

```scheme
; Modbus ASCII over UDP: one :...CR LF frame per datagram; replies go to the sender's address
(server (mode ascii-udp) (port 1502) (bind "127.0.0.1"))
```

`ascii-udp` accepts only `mode`, `port`, `bind` and `seed`; `tls`, `idle-timeout-ms`, serial line
options, RTU timing options and `broadcast-execute` are rejected at load time. Unit ids follow the
RTU/ASCII rule (1-247; 0 is broadcast and never answered). A datagram must hold exactly one complete
frame; anything else (no CR LF, extra bytes, two frames, bad LRC) is ignored and counted as an FC08
`0x000C` communication error on every device, and empty or longer than 515 byte datagrams are
dropped without counting.

```scheme
; Modbus RTU frames, one per UDP datagram; replies go to the sender's address
(server (mode rtu-udp) (port 1502) (bind "127.0.0.1") (broadcast-execute off))
```

`rtu-udp` accepts only `mode`, `port`, `bind`, `seed` and `broadcast-execute`; `tls`,
`idle-timeout-ms` (no connections), `baud`, `frame-gap-ms` and `strict-t15` (datagram boundaries
delimit frames, so there is no t3.5/t1.5 timing) and the serial line options are rejected at load
time. Unit ids follow the `rtu-tcp` rule (1-247). A datagram must hold exactly one valid RTU frame
(CRC checked); otherwise it is ignored.

```scheme
(server (mode tcp) (bind "127.0.0.1")
  (tls (certificate "/etc/modbus/server.pem")
       (private-key "/etc/modbus/server.key")
       (ca-certificate "/etc/modbus/ca.pem")
       (handshake-timeout-ms 5000)))
```

### TLS (Modbus TCP over TLS)

`(tls ...)` wraps the same 7-byte MBAP header and PDU in TLS on `tcp` listeners; no Modbus framing
or function-code behavior changes. It is allowed only in `tcp` mode and is rejected at load time,
including by `--check`, in every other mode (even as an empty `(tls)` form). A binary built with
`WITH_TLS=0` has no OpenSSL support: `(tls ...)` is rejected at load time in every mode with the
error `built without TLS support`. Plaintext TCP is unchanged when `(tls ...)` is absent, in either
build.

| Entry | Meaning | Default |
|---|---|---|
| `(certificate "PATH")` | Server certificate chain in PEM format. Required, 1-255 characters | none |
| `(private-key "PATH")` | Server private key in PEM format. Required, 1-255 characters. Its contents are never logged | none |
| `(ca-certificate "PATH")` | CA certificate used to verify client certificates and advertised as the acceptable client CA list, 1-255 characters | none |
| `(handshake-timeout-ms N)` | Close a TLS connection whose handshake is not complete within `N` milliseconds after accept, 1-86400000. Independent of `(idle-timeout-ms)` | `10000` |
| `(require-client-certificate on\|off)` | Require a client certificate signed by the configured CA | `on` |
| `(min-version tls1.2\|tls1.3)` | Minimum TLS version | `tls1.2` |
| `(tls1-2-ciphers "LIST")` | OpenSSL TLS 1.2 (and below) cipher list; must be non-empty, at most 511 characters | OpenSSL default |
| `(tls1-3-ciphersuites "LIST")` | OpenSSL TLS 1.3 cipher suite list; must be non-empty, at most 511 characters | OpenSSL default |

`ca-certificate` is required while `require-client-certificate` is on (the default). Set it to
`off` and omit the CA to simulate anonymous clients; this is a testing relaxation, not a
recommended production setting. Paths are checked for non-emptiness during config load. When TLS is
enabled, both `--check` and server startup create the OpenSSL context: a missing or unreadable
certificate/key/CA, a CA whose name list cannot be loaded, a mismatched certificate and private key,
or an invalid cipher list makes `--check` fail with exit status 1 and no `config OK` line (startup
fails with the same error). TLS
1.0, TLS 1.1, SSLv3 and anonymous/NULL ciphers are always disabled, and user cipher strings cannot
lower the configured minimum. The configured CA is advertised to clients as the acceptable client CA
list, but revocation checking (CRL/OCSP) and Modbus Security role authorization from x.509 role OIDs
are not implemented: a trusted client certificate is either required or optional for every device.

Each accepted TLS connection has a handshake deadline measured from `accept()`, independent of
`(idle-timeout-ms)` (which still defaults to `0`). A client that leaves the handshake unfinished is
closed at the deadline and logged at `WARN` with its peer address and the configured timeout, freeing
its slot for other clients. Connections that completed the handshake are not affected; normal TLS
requests and delay faults keep their existing timing. A client certificate's subject is logged at
`INFO` once the handshake succeeds. When the server closes a TLS connection (idle timeout, I/O
error, EOF, or shutdown), it makes one nonblocking best-effort attempt to send `close_notify` before
freeing the session, so clients can observe a clean TLS close without delaying other clients. An
ungraceful peer drop reported with an empty OpenSSL error queue is treated as EOF and is not logged
as a TLS failure.

### Framing and serial lines

RTU frame boundaries use baud-derived t3.5 when baud is known (`(baud N)` or the tty's current
rate), unless `(frame-gap-ms N)` overrides it; with no known baud the fallback is 20 ms.
`strict-t15` is a best-effort RTU byte-stream-only heuristic in baud-derived auto mode and never
applies to RTU-over-TCP.

ASCII is `:` + hex `[addr][PDU][LRC]` + `CR LF`, delimited by the terminator rather than silence;
it has no RTU timing options. Inter-frame `CR`, `LF`, and spaces are ignored; a trailing `CR` waits
for `LF`; a second `:` resynchronizes; a partial frame with no byte for 1 s is dropped. Rejected or
undecodable frames increment FC08 `0x000C` on every device. Serial ASCII ttys default to 8N1;
`(data-bits 7) (parity even) (stop-bits 1)` selects 7E1 but does not change framing. `ascii-tcp`
carries the same frames over TCP and accepts only `mode`, `port`, `bind`, `seed`, and
`idle-timeout-ms`. All ASCII modes ignore unit-0 broadcasts and reject `broadcast-execute`.

## `(device ...)`

| Entry | Meaning | Default |
|---|---|---|
| `(name "Text")` | Name shown in logs and `--check`, up to 63 characters | `"unnamed"` |
| `(unit-id N)` | Modbus unit id (slave address). TCP: 0-255. RTU/ASCII: 1-247 (`0` is broadcast, `248-255` reserved by Modbus serial spec). Required, at most once per device | required |
| `(exception-status N)` | Byte returned by Read Exception Status (FC 07), 0-255 | `0` |
| `(server-id N)` | Server ID byte returned by Report Server ID (FC 17), 0-255 | `0` |
| `(run-indicator N)` | Run Indicator Status byte returned by Report Server ID (FC 17), 0-255 (`0xFF` ON/run, `0x00` OFF by convention) | `255` |
| `(server-id-additional-data "HEXBYTES")` | Additional Data returned by Report Server ID (FC 17): even-length hex string, empty allowed, 0-249 bytes (`""` = none) | `""` |
| `(com-status N)` | Status returned by Get Com Event Counter (FC 11) and Get Com Event Log (FC 12), `0` (ready) or `65535` (busy) | `0` |
| `(com-event N)` | One FC 12 event byte, 0-255. Repeatable up to 64 entries; reported in configuration order | none (empty log) |
| `(vendor-name "Text")` | MEI 14 Read Device Identification object 0x00, 0-244 bytes | `"mogbus"` |
| `(product-code "Text")` | MEI 14 object 0x01, 0-244 bytes | `"mogbus"` |
| `(major-minor-revision "Text")` | MEI 14 object 0x02, 0-244 bytes | `"1.0"` |
| `(vendor-url "Text")` | MEI 14 object 0x03, 0-244 bytes; setting any regular object reports regular conformity (0x82) | absent |
| `(product-name "Text")` | MEI 14 object 0x04, 0-244 bytes | absent |
| `(model-name "Text")` | MEI 14 object 0x05, 0-244 bytes | absent |
| `(user-application-name "Text")` | MEI 14 object 0x06, 0-244 bytes | absent |
| `(device-id-object ID "Text")` | Repeatable extended Read Device Identification object; `ID` is decimal 128-255 (`0x80-0xFF`), text is 1-244 bytes | none |
| `(note "Text")` | A comment kept for humans. Can be repeated. Ignored by the program | - |
| `(fault ...)` | Repeatable fault, one definition per kind, see below | none |
| `(file ...)` | File record data for Read/Write File Record (FC 20/21), see below | none |
| `(fifo ...)` | FIFO queue data for Read FIFO Queue (FC 24), see below | none |
| `(canopen-object ...)` | Repeatable CANopen object used by FC 43 MEI `0x0D` SDO uploads/downloads, see below | none |
| `(function-code CODE ...)` | Repeatable user-defined function code (up to 8 per device), see below | none |
| `(machine NAME ...)` | Repeatable scripted state machine, see below (max 8 per device) | none |
| `(coil ...)` `(discrete ...)` `(input ...)` `(holding ...)` | Data points, see below | - |

Unit ids must be unique across devices. In `rtu-pty`, `rtu-device`, `rtu-tcp`, `ascii-pty`,
`ascii-device` and `ascii-tcp` modes, unit ids 0 and 248-255 are rejected at startup and by
`--check` because address 0 is broadcast and 248-255 are reserved by the Modbus serial specification.

### Read Device Identification (FC 43)

The mandatory objects and optional regular objects use `(vendor-name "Text")`,
`(product-code "Text")`, `(major-minor-revision "Text")`, `(vendor-url "Text")`,
`(product-name "Text")`, `(model-name "Text")`, and `(user-application-name "Text")`.
Extended objects use this exact syntax and may be repeated in any order:

```scheme
(device (unit-id 1)
  (holding 0)
  (device-id-object 128 "First extended object")
  (device-id-object 255 "Last extended object"))
```

`ID` must be a decimal integer from 128 to 255; text must be non-empty and at most 244 bytes.
IDs must be unique and at most 32 extended objects may be configured per device; entries are
served in ascending ID order. Configuring any extended object raises FC 43 Conformity Level to
`0x83`. MEI `0x0E` is the Read Device Identification type; FC 43 with MEI `0x0D` performs
CANopen SDO uploads and downloads against the objects configured with `(canopen-object ...)`.
See [modbus-support.md](modbus-support.md) for response semantics.

### CANopen objects (FC 43 MEI `0x0D`)

CANopen SDO uploads and downloads use objects configured with this repeatable form:

```scheme
(canopen-object NODE INDEX SUBINDEX "HEXBYTES")
```

- `NODE` is the CANopen node id, 1-127.
- `INDEX` is 0-65535 and `SUBINDEX` is 0-255; both accept decimal or hex such as `#x6041`.
- `HEXBYTES` is 2-476 hex digits (1-238 bytes), with upper- or lowercase digits.

Each `(NODE, INDEX, SUBINDEX)` must be unique on a device, with at most 64 objects per device.
Every configured object is readable and writable through MEI `0x0D` SDO transfers, at any byte
offset within its configured data. Undefined objects and out-of-range transfers are rejected.

```scheme
(device (unit-id 1)
  (canopen-object 5 #x6041 0 "17020000")
  (canopen-object 5 #x6041 1 "AABB"))
```

In `rtu-pty`, `rtu-device`, `rtu-tcp`, `ascii-pty`,
`ascii-device` and `ascii-tcp` modes, unit ids 0 and 248-255 are rejected at startup and by
`--check` because address 0 is broadcast and 248-255 are reserved by the Modbus serial specification.

## Data points

There are four Modbus tables. Use the one your real device uses.

| Entry | Table | Holds | Can a client write it? | Function codes |
|---|---|---|---|---|
| `coil` | Coils | on/off | yes | 01, 05, 15 |
| `discrete` | Discrete inputs | on/off | no | 02 |
| `input` | Input registers | numbers | no | 04 |
| `holding` | Holding registers | numbers | yes | 03, 06, 16, 22, 23 |

Shape of a point:

```scheme
(holding ADDRESS
  (type ...)          ; registers only, default u16
  (word-order ...)    ; 32- and 64-bit types only, default high-first
  (byte-order ...)    ; registers only, default big
  (length N)          ; (type string) only: registers, 1-123
  (padding ...)       ; (type string) only: nul (default) or space
  (static VALUE)      ; or (ramp ...), (sine ...) or (lag ...): only one. None: 0 / off
  (noise ...)         ; optional, registers only: added to any of the above
  (degradation ...)   ; optional: drift, noise growth, stuck and fail over time
  (access ...)        ; coil and holding only: read-write (default) or read-only
  (count N)           ; optional: repeat for N consecutive points
  (note "Text"))      ; optional comment
```

`ADDRESS` is the Modbus address as the client sends it on the wire (0-65535). If your device's
manual says "40001", that is address `0` of the holding table; "40101" is `100`.

**Addresses you do not define do not exist.** A client reading or writing one gets exception 02
(illegal data address), like a real device. A read that covers a gap also fails.

### `(type ...)`

| Type | Registers used | Range |
|---|---|---|
| `u16` | 1 | 0 to 65535 |
| `s16` | 1 | -32768 to 32767 |
| `u32` | 2 | 0 to 4294967295 |
| `s32` | 2 | -2147483648 to 2147483647 |
| `float32` | 2 | IEEE 754 single precision |
| `u64` | 4 | 0 to 18446744073709551615 |
| `s64` | 4 | -9223372036854775808 to 9223372036854775807 |
| `float64` | 4 | IEEE 754 double precision |
| `string` | `(length N)` | N registers of text, see below |

Coils and discrete inputs are always on/off; do not give them a `type`.
Integer register values are rounded to the nearest whole number and limited to the range of the type.
`float32` and `float64` can represent `NaN`, `+inf.0`, and `-inf.0` (useful as invalid-sensor
sentinels). Finite `float32` values outside the representable float range become `+inf.0`/`-inf.0`
instead of causing undefined behavior. Non-finite static values are accepted only for the float
types; integer and on/off static values must be finite.

A multi-register point occupies consecutive addresses: `(input 4 (type u32) ...)` uses addresses 4
and 5, `(input 4 (type u64) ...)` uses 4 to 7.

**64-bit precision.** Values are doubles inside mogbus (ramps, models, machines, `get_value`), so
`u64` and `s64` integers above 2^53 (about 9.007e15) lose precision there. Raw register reads and
writes by clients are exact, and so is a `(static N)` integer: s7 reads integers as 64-bit, so
`(static 9223372036854775807)` and `(static -9223372036854775808)` encode exactly. A `u64` static
above 2^63-1 must be written in hex, `(static #xFFFFFFFFFFFFFFFF)` (s7 has no decimal literal for it);
for `u64` an integer literal is taken as 64-bit two's complement, so a negative one wraps instead
of clamping. A static with `(noise ...)` or `(degradation ...)` is a model and goes through doubles.

### `(word-order ...)`

For 32- and 64-bit types: which register holds the most significant part.

- `high-first` (default): the first register is the most significant. This is the usual "big-endian" layout.
- `low-first`: the registers come in reverse order, the first one is the least significant. Some
  devices do this ("word swapped").

### `(byte-order ...)`

For every register type (16, 32 and 64-bit, and strings): `big` (default) puts the high byte of
every register first on the wire, as Modbus does. `little` swaps the two bytes inside every
register of the point. It is applied after the word order, so the two options combine into the four
layouts real 32-bit meters use (bytes A B C D from most to least significant):

| Layout | `word-order` | `byte-order` | Registers (float32 1.5 = `3F C0 00 00`) |
|---|---|---|---|
| ABCD | `high-first` | `big` | `3FC0 0000` |
| CDAB | `low-first` | `big` | `0000 3FC0` |
| BADC | `high-first` | `little` | `C03F 0000` |
| DCBA | `low-first` | `little` | `0000 C03F` |

For 64-bit types the same two options apply to eight bytes A..H: `high-first`/`big` is ABCDEFGH,
`low-first`/`big` is GHEFCDAB (registers reversed), `high-first`/`little` is BADCFEHG (every
register swapped) and `low-first`/`little` is HGFEDCBA (the whole byte string reversed). For
16-bit types only `byte-order` has an effect (`little` turns `0x1234` into `0x3412`).

### Strings

`(type string) (length N)` is text stored in N registers (1-123) of two ASCII bytes each; the first
character is the high byte of the first register, so a client sees it in reading order. The
`byte-order little` option swaps the two characters inside every register. `(static "TEXT")` sets the
text, at most `2*N` bytes of printable ASCII (0x20-0x7E) in the config; a shorter text is padded
with NUL bytes, or with spaces when you add `(padding space)`.

```scheme
(input 20 (type string) (length 8) (static "SN-000417"))              ; 16 bytes, NUL padded
(input 28 (type string) (length 8) (padding space) (static "EM-100")) ; space padded
(holding 40 (type string) (length 4))                                 ; empty, writable
```

Strings can only be `holding` or `input` points and only static: no `ramp`, `sine`, `lag`,
`noise` or `degradation`, no `word-order`. A client may write them (holding table) with raw
register writes, which changes the text until the device restarts; `restart` restores the configured
text. `(count N)` repeats the whole string point. Machines cannot read, drive or watch strings
(config load fails). The C API and the Python package read and set the text with
`mogbus_device_get_string()`/`set_string()` (see [library.md](library.md)); `get_value` fails on a
string point. Text read back stops at the first NUL byte; a client may write any byte value.

### `(static VALUE)`

The value never changes by itself.

```scheme
(holding 0 (static 230))
(input 2 (type float32) (static 0.97))
(coil 0 (static on))
```

### `(ramp ...)`

The value moves from `from` towards `to`, by `step`, once every `every-ms` milliseconds.

| Entry | Meaning | Default |
|---|---|---|
| `(from N)` | first value | required |
| `(to N)` | last value | required |
| `(step N)` | how much it changes each time. Always positive; the direction follows from `from` and `to` | `1` |
| `(every-ms N)` | milliseconds between steps, 1-86400000 | `1000` |
| `(mode ...)` | what happens at the end, see below | `wrap` |

Modes:

- `wrap`: jumps back to `from` and starts again (a sawtooth). `0 10 20 30 0 10 ...`
- `bounce`: turns around and walks back (a triangle). `0 10 20 30 20 10 0 10 ...`
- `hold`: stays on the last value. `0 10 20 30 30 30 ...`

The value never goes beyond `to`. If the step does not divide the range evenly, the last value
is the last one that fits: from 0 to 25 by 10 gives `0 10 20`.

Ramps start when the simulator starts, and every ramp in the file starts at the same moment.
Ramps are not available for coils and discrete inputs.

```scheme
(input 0 (type s16)
  (ramp (from 180) (to 240) (step 2) (every-ms 3000) (mode bounce)))
```

### `(sine ...)`

The value is a sine wave of the time since the simulator started:
`offset + amplitude * sin(2 pi * t / period + phase)`. Registers only.

| Entry | Meaning | Default |
|---|---|---|
| `(offset N)` | centre value | required |
| `(amplitude N)` | peak distance from the centre (any finite number) | required |
| `(period-ms N)` | milliseconds per cycle, 1-86400000 | required |
| `(phase-deg N)` | starting angle in degrees | `0` |

```scheme
(input 1 (type s16) (sine (offset 200) (amplitude 30) (period-ms 86400000)))
```

### `(lag ...)`

A first-order lag: the value moves towards its goal `gain * target + offset` and closes
`1 - 1/e` (63%) of the remaining distance every `tau-ms`. It is the "heater following its
setpoint" model. Registers only.

| Entry | Meaning | Default |
|---|---|---|
| `(target X)` | a number, or a point to follow: `(holding 1)`, `(input 4)`, `(coil 0)`, `(discrete 0)`. The point must be a configured point (first address); bits count as 0 or 1 | required |
| `(tau-ms N)` | time constant, 1-86400000 | required |
| `(initial N)` | value at start and after a restart | the goal |
| `(gain N)` | multiplier of the target | `1` |
| `(offset N)` | added to `gain * target` | `0` |

The target may be declared before or after the point, but not be the point itself. The value
is computed when a request arrives, with the exact solution over the time since the previous
evaluation, so it does not depend on how often clients read. The target is read at that moment:
a target that changes between two evaluations (for example a client writing the setpoint) is
sampled at those times, and every request, reads and writes alike, evaluates the lag first.

```scheme
(holding 1 (type s16) (static 300))     ; setpoint, writable
(input 0 (type s16) (lag (target (holding 1)) (tau-ms 60000) (initial 200)))
```

### `(noise ...)`

Adds a pseudo-random offset to a `static`, `ramp`, `sine` or `lag` value. Registers only, and
there must be one of those behaviours in the same point.

| Entry | Meaning | Default |
|---|---|---|
| `(amplitude N)` | `uniform`: offset in `[-N, +N]`; `gaussian`: standard deviation `N`, limited to +-4N. 0 or more | required |
| `(every-ms N)` | a new sample every N milliseconds (held in between), 1-86400000 | `1000` |
| `(distribution ...)` | `uniform` or `gaussian` | `uniform` |

The sample is a fixed function of the device unit id, the point's position in the device and
`floor(time since start / every-ms)`: it is reproducible between runs, ignores `--seed`, does
not depend on how often clients read, and two points get different sequences. For a `lag` the
noise is added to the output and not fed back into the lag. Values go through the point's `type`
afterwards: integer types round to the nearest and clamp to their range.

```scheme
(input 2 (type s16)
  (sine (offset 200) (amplitude 30) (period-ms 600000))
  (noise (amplitude 2) (every-ms 500) (distribution gaussian)))
```

### `(degradation ...)`

Ages a sensor: its value drifts, gets noisier, sticks and finally fails. It follows the behaviour
(and the optional `(noise ...)`) in the same point, which must have a `static`, `ramp`, `sine` or
`lag`. All times are milliseconds **since the device started** (0-4294967295), so a restart,
whether a `restart` fault or FC 08 restart communications, repairs the sensor and the timeline
begins again.

| Entry | Meaning | Default |
|---|---|---|
| `(after-ms N)` | drift and noise growth start at N ms; before that the point is healthy | `0` |
| `(drift-per-hour D)` | adds `D * (t - after) / 1 hour` to the value once `t > after`; any finite number, negative drifts down. Registers only | none |
| `(noise-growth-per-hour G)` | adds `G * (t - after) / 1 hour` to the noise amplitude (G is 0 or more). The point's own `(noise ...)` keeps its every-ms and distribution; without one the extra noise is uniform with a new sample every 1000 ms. Registers only | none |
| `(stuck-after-ms S)` | from S on the value stays at what it was (drift and noise included) at the first evaluation at or after S, whatever the base behaviour does afterwards | none |
| `(fail-after-ms F)` | from F on, reads that cover an address of this point answer exception 04 | none |

At least one of `drift-per-hour`, `noise-growth-per-hour`, `stuck-after-ms` and `fail-after-ms`
is required. `stuck-after-ms` and `fail-after-ms` also work on `coil` and `discrete` points
(which must be `(static on/off)`); drift and noise growth are for registers only. Order in the
output: base value, drift, noise (with its growth), then stuck. The result goes through the
point's `type` like any other value.

A failed point only answers exception 04 to reads: FC 01, 02, 03, 04, 23 and a user-defined
`(registers ...)` action whose range covers it (the exception is checked after the address check,
so an undefined address still answers 02, and FC 23 does not perform its write when its read
fails). Writes are unaffected: a client can still write a failed holding point or coil. Failed
reads count as exceptions in the FC 08 diagnostic counters and in the metrics.

A client write, or an embedding host claiming the point, removes the model effects like for any
modeled point (see below): drift, noise and stuck stop, and the written value is returned. The
failure does not stop: `fail-after-ms` stays configured until the device restarts, so a written
point that has failed keeps answering exception 04. Machines cannot drive a point with
`(degradation ...)`.

```scheme
(input 0 (type u16)
  (sine (offset 500) (amplitude 40) (period-ms 600000))
  (noise (amplitude 1))
  (degradation
    (after-ms 3600000)           ; healthy for the first hour
    (drift-per-hour 6)           ; then +6 per hour
    (noise-growth-per-hour 4)    ; and noisier by 4 per hour
    (stuck-after-ms 10800000)    ; frozen at 3 h
    (fail-after-ms 21600000)))   ; reads fail with exception 04 at 6 h
(coil 0 (static on) (degradation (fail-after-ms 60000)))
```

See [`profiles/aging-sensor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/aging-sensor.scm).

### What happens when a client writes

Clients can write `coil` and `holding` points. The written value is stored and returned by
later reads. If the point was a ramp, sine or lag (or had noise or degradation), **it stops moving** and keeps the written value as a
static value until the simulator is restarted. Machine-owned points, including state registers,
reject client writes with exception 02 (see `(machine ...)` below).

### `(access ...)`

`(access read-only)` makes a coil or holding point refuse every Modbus write while clients can still
read it, like a status word or a counter in a real device. `(access read-write)` is the default.
A write (FC 05, 06, 15, 16, 22 or the write part of FC 23) that touches any address of a read-only
point fails as a whole with exception 02, exactly like a write to an undefined address: nothing is
written, and a ramp, sine or lag on the point keeps moving because clients cannot freeze it.
Machines, `(set! ...)` and the host side of the [library](library.md) and the HTTP control API
(`PUT` of raw values and `/value`) can still change the point. `(access ...)` is rejected on `input`
and `discrete` points, which are always read-only for clients.

```scheme
(holding 0 (access read-only) (static #x0001))     ; status word
(holding 10 (access read-only) (ramp (from 0) (to 65535) (step 1) (every-ms 1000) (mode wrap)))
```

### `(count N)`

Repeats the point `N` times at consecutive addresses (the step is the point's width: 1 for
16-bit types, 2 for 32-bit types, 4 for 64-bit types, `length` for strings). All copies get the same value, ramp or model (a lag target is not shifted). A device can have at
most 8192 points total (`DEVICE_MAX_POINTS`); a count that would exceed this is rejected.

```scheme
(coil 0 (count 16))                         ; coils 0..15, all off
(holding 10 (type u32) (count 3) (static 7)) ; addresses 10-11, 12-13, 14-15
```

## `(machine ...)`

A device can own up to eight scripted state machines. They are read by s7 as data at config load,
compiled into flat expression/statement arrays and copied into the device arena. At request time
`device_tick` only advances them: no s7 evaluation and no allocation.

```scheme
(machine NAME
  (initial STATE)
  (state-register (holding ADDR))    ; optional, immediately after (initial STATE)
  (var NAME LITERAL) ...             ; zero or more, before any (state ...)
  (state NAME                        ; one or more, before any (transition ...)
    (on-enter STATEMENT ...)
    (on-tick STATEMENT ...)
    (on-exit STATEMENT ...)
    (fault (kind K) ...)) ...        ; per-state faults, one definition per kind
  (on-write POINT STATEMENT ...) ... ; after states, before transitions
  (transition FROM TO TRIGGER ...) ...)
```

Handlers are optional and may appear in any order within a state. Faults may also appear in a state,
using the same options as the device-level `(fault ...)` described below.

Names use lowercase letters, digits and `-`, start with a letter, and are at most 63 characters.
Machine names are unique per device; state and variable names are unique per machine.
`(initial STATE)` is required and must name a state. Entries must be ordered as shown: optional
state register, variables, states, on-write handlers, then transitions. Each handler can appear at
most once per state.

Statements (usable in handlers and nested in guards):

```scheme
(set! TARGET EXPR)
(when EXPR STATEMENT ...)
(unless EXPR STATEMENT ...)
```

`TARGET` is a variable name or a point accessor. A guard runs its body when its condition is true
(`when`) or false (`unless`); guards can nest.

Expressions are finite numbers (`0`, `1200`, `0.5`), booleans (`#t`, `#f`), a variable symbol, a
point accessor, `(now-ms)`, `(elapsed-ms)`, exactly-two-operand `(+ A B)`, `(- A B)`, `(* A B)`,
`(/ A B)`, `(min A B)`, `(max A B)`, `(< A B)`, `(<= A B)`, `(> A B)`, `(>= A B)`, `(= A B)`,
`(not A)`, or `(if COND THEN ELSE)`. `(and A B ...)` and `(or A B ...)` take at least two
operands and short-circuit. `(now-ms)` is the current monotonic time; `(elapsed-ms)` is the time
since this machine last ticked or changed state.

There are exactly two types: **number** and **boolean**. There is no implicit conversion:
arithmetic (`+`, `-`, `*`, `/`, `min`, `max`) and ordering comparisons (`<`, `<=`, `>`, `>=`) need
numbers; `=` needs two values of the same type; `if` conditions, guards (`when`/`unless`), `not`,
`and`, `or` and transition `(when ...)` conditions need booleans; `if` branches and `set!` need
matching types. Integer and real literals are both numbers; a value assigned to a point is
rounded/clamped to that point's configured type.

Point accessors are `(coil ADDR)`, `(discrete ADDR)`, `(input ADDR)` and `(holding ADDR)`. Reads go
through the configured point, so bit tables yield booleans and registers yield numbers with the
point's type and word order applied. Both reads and `set!` targets must name the **first address of
a configured point**; an undefined address or a continuation address of a 32-bit point is a load
error. A machine can `set!` a static point but not a ramp, sine or lag point or a state-register point, and a
point may be driven by at most one machine. A machine-driven point stays machine-owned: a client
write to any address of it fails with exception 02 and leaves the value unchanged, so the machine
keeps driving it. Machine/internal writes still update the point normally.

`(state-register (holding ADDR))` or `(state-register (input ADDR))` is optional, at most once per
machine, and must be the first item after `(initial STATE)`. It names the first address of a
configured static register point (not a ramp and not driven by any machine). That point becomes
machine-owned and read-only to clients; it holds the zero-based index of the current state as a
number. It is written before the initial state's `on-enter` and after each state switch, before the
new state's `on-enter`, and is readable from machine expressions and Modbus reads.

`(on-write POINT STATEMENT ...)` is optional, repeatable (at most 16 per machine), and must follow
all states and precede transitions. `POINT` names the first address of a configured point that is
not machine-driven; at most one handler per point is allowed in one machine. The handler runs
immediately after a successful client write that touches any register of that point, after the raw
value is written and in the same request. It sees the just-written value and may read/set variables
and points like other handlers, but it does not fire transitions or advance any machine. Failures
in an on-write handler are logged with the machine's current state. Handlers in different machines
run in declaration order. `(now-ms)` is the request's tick time and `(elapsed-ms)` is measured from
the machine's last tick or state change.

A `(fault ...)` inside `(state ...)` overlays the device faults while that state is active. It uses
the same options and one-definition-per-kind rule as a device-level fault, and a `(kind disconnect)`
overlay is rejected in serial modes for the same reason. A checksum-corrupt overlay is rejected in
`tcp` mode even when that state is never entered. An active overlay overrides or adds its kind;
kinds not mentioned keep the device setting. If several machines are in states that define the
same kind, the earliest declared machine wins for that kind.

Machines on the same device share one point table: any machine can read any configured point,
including a point driven by another machine, but a point has at most one machine writer, and
machines advance in declaration order on each tick, so earlier writes are visible to later machines
in the same tick and later writes are visible to earlier machines on the next tick.

Each transition has at least one trigger and at most one `(when EXPR)` and one `(after-ms ...)`:

```scheme
(transition FROM TO (when EXPR) (after-ms MS))
(transition FROM TO (after-ms (random MIN MAX)))
```

`MS` is a whole number from 1 to 86400000 (24 hours). `(random MIN MAX)` needs whole-number bounds
with `1 <= MIN <= MAX <= 86400000`; the deadline is drawn from that inclusive range. An `after-ms`
transition with no `when` fires at its deadline; a `when` transition fires at the current time,
gated by its optional deadline. Due after-only transitions win, earliest deadline first (ties in
declaration order), then `when` transitions are tested in declaration order; the first match fires.
Firing runs the old state's `on-exit`, switches state, redraws all deadlines from the firing time,
then runs the new state's `on-enter`. If no transition fires, one `on-tick` runs. A machine fires at
most 64 transitions per tick. When one tick catches up through several due transitions, intermediate
states get `on-exit`/`on-enter` but no `on-tick`; only the final state gets one `on-tick` at request
time (none if the 64-firing cap is reached). Ramps or accumulators that need to advance through
skipped states should update in `on-exit` using `elapsed-ms` instead.

All `(machine ...)` items are compiled while each device is read; points may be declared before or
after them. At the end of that `(device ...)`, config checks deferred point references. After all
top-level forms are read, it seeds each machine RNG from `(seed N)` in `(server ...)` (mixed with the
device unit id) and finalizes: it validates types and point ownership, draws initial deadlines and
runs the initial state's `on-enter` at simulator start time. With no `(server ...)` form the seed is
0. Because seeding happens after all forms, the position of `(server ...)` does not matter. The same
seed and unit id therefore give the same random deadlines on every run.

Limits are `MACHINE_MAX_MACHINES` 8 per device, `MACHINE_MAX_STATES` 16,
`MACHINE_MAX_TRANSITIONS` 32, `MACHINE_MAX_VARS` 16, `MACHINE_MAX_WRITE_HANDLERS` 16 per machine,
`MACHINE_MAX_STATEMENTS` 128 (including nested statements), `MACHINE_MAX_EXPR_NODES` 512 and
`MACHINE_MAX_FIRINGS` 64 per tick. A machine has at most one state register.

```scheme
(device
  (unit-id 1)
  (coil     0)
  (coil     1)
  (discrete 0)
  (input    0 (type u16))
  (holding  0 (type u16) (static 1200))

  (machine motor
    (initial idle)
    (var speed 0)

    (state idle
      (on-enter (set! (discrete 0) #f) (set! speed 0) (set! (input 0) 0)))

    (state starting
      (on-tick (set! speed (min (holding 0) (+ speed (/ (* 40 (elapsed-ms)) 1000))))
               (set! (input 0) speed)))

    (state running
      (on-enter (set! (discrete 0) #t))
      (on-tick  (set! (input 0) (holding 0))))

    (state tripped
      (on-enter (set! (discrete 0) #f) (set! speed 0) (set! (input 0) 0)))

    (transition idle     starting (when (coil 0)))
    (transition starting running  (after-ms 3000))
    (transition running  idle     (when (not (coil 0))))
    (transition running  tripped  (after-ms (random 60000 600000)))
    (transition tripped  idle     (when (coil 1)))))
```

See also [`profiles/motor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/motor.scm).

## `(fault ...)`

A device can have several faults. `(fault ...)` is repeatable, but **only one definition per
kind** is allowed: a second `(fault (kind X) ...)` for the same kind is rejected at config load.
A fault fires either by its `percent` chance or by its `schedule`; the two are mutually exclusive.

```scheme
(fault
  (kind delay|drop|exception|corrupt|disconnect|restart|sensor-disconnect)
  [(delay-ms N)]                  ; delay only
  [(code N)]                      ; exception only
  [(mode pdu|register|checksum)]  ; corrupt only, default pdu
  [(percent N)]                   ; not allowed together with schedule
  [(schedule (step (after-ms N) (count N)) ...)])
```

| Kind | Effect |
|---|---|
| `delay` | Send the response late. |
| `drop` | Never send the response. |
| `exception` | Replace the response with a Modbus exception. |
| `corrupt` | Corrupt the response in the selected `mode`. |
| `disconnect` | Drop the response and close the client connection (TCP-like transports only). |
| `restart` | Restart the device after the response is built. |
| `sensor-disconnect` | Replace successful FC 02 and FC 04 reads with exception 04. |

| Entry | Meaning | Default |
|---|---|---|
| `(kind NAME)` | Required. One of the kinds above. | - |
| `(delay-ms N)` | For `delay` only: how late, 1-60000 ms. Required for `delay`, rejected for other kinds. | - |
| `(code N)` | For `exception` only: exception code to return, one of 1-8, 10 or 11 (decimal). Required for `exception`, rejected for other kinds; 9 is reserved. | - |
| `(mode pdu\|register\|checksum)` | For `corrupt` only, see below. `pdu` flips one PDU payload byte; `register` flips a register data value in FC 03/04/23 read responses and the echoed value of FC 06; `checksum` corrupts the emitted RTU CRC or ASCII LRC after framing. Rejected for other kinds. | `pdu` |
| `(percent N)` | Chance per response, 0-100. Not allowed together with `schedule`. | `100` |
| `(schedule (step ...) ...)` | Non-repeating schedule of at most 8 steps; each step needs `(after-ms N)` (0-86400000) and `(count N)` (1-1000000). Not allowed together with `percent`. | none (percent roll) |

Schedule timing: the first step becomes active `after-ms` after device start (or, for a machine
state overlay, after that overlay became active). Each later step
becomes active `after-ms` after the request that completes the previous step. A step fires exactly
`count` consecutive requests, then the next step waits; after the last step the schedule stops.
Schedules never repeat. Progress restarts when the active fault definition or schedule changes (for
example a machine state overlay becoming active) and on a device restart, except that the restart
kind keeps its own progress so a scheduled restart does not re-arm itself. A different source
counts as a change even with an identical schedule: switching between the device definition and a
state overlay, between two overlays, or re-entering a state restarts progress. Requests that
produce no response (broadcast-execute unit-0 requests, listen-only mode) do not consume schedule
steps.

`corrupt` modes: `pdu` (the default) flips `resp[1]` (the byte count or first data byte; the
function code for a one-byte response) with XOR `0xFF`, so the response length does not change.
`register` changes a register data value in successful FC 03/04/23 read responses and the echoed
value of FC 06; it leaves other response shapes unchanged. `checksum` deliberately corrupts the
RTU CRC or ASCII LRC after the frame is built, so the receiver rejects the frame. MBAP/TCP has
no checksum, so `checksum` is rejected in `tcp` mode and allowed in `rtu-pty`, `rtu-device`,
`rtu-tcp`, `ascii-pty`, `ascii-device`, `ascii-tcp`, `ascii-udp` and `rtu-udp`; it is rejected in `udp` for the same reason.

```scheme
(fault (kind delay) (delay-ms 1500) (percent 30))   ; 3 in 10 answers arrive 1.5 s late
(fault (kind drop) (percent 20))                    ; 1 in 5 answers never arrives
(fault (kind exception) (code 4) (percent 100))     ; every answer becomes exception 04
(fault (kind corrupt) (mode register) (percent 50)) ; half of the register reads carry a wrong value
(fault (kind disconnect) (percent 10))              ; 1 in 10 answers closes the connection
(fault (kind sensor-disconnect)                     ; sensor reads fail during 600 requests
  (schedule (step (after-ms 5000) (count 600))))    ; after 5 s, then recover
(fault (kind restart)                               ; one restart on the first request after 60 s
  (schedule (step (after-ms 60000) (count 1))))
```

A state machine may also put `(fault ...)` forms inside `(state ...)`; these are per-state overlays.
See the `(machine ...)` section above for the full overlay rules; an overlay changes the active
fault definition and therefore restarts that kind's schedule progress.

`(kind disconnect)` is valid only for TCP-like transports (`tcp`, `rtu-tcp`, `ascii-tcp`).
It is rejected at load time in `rtu-pty`, `rtu-device`, `ascii-pty`, `ascii-device`, `udp`, `ascii-udp` and `rtu-udp`
(serial server fds are never closed by a fault, and UDP has no connection). `(kind corrupt)
(mode checksum)` is rejected in `tcp` and `udp` and accepted in the other eight modes. In every UDP mode, a
`delay` fault holds the reply in a 16-slot table; when it is full the response is dropped. `mode` is accepted only for `corrupt`; `delay`, `drop`,
`exception`, `restart` and `sensor-disconnect` are valid in every mode.

- Delays do not block the server: other clients are served while a response waits. A configured
  delay is capped at 60000 ms.
- A dropped response means the client sees a timeout. The write or read itself **still
  happens** on the device, like a real device whose answer got lost.
- A disconnect implies a dropped response and additionally closes the connection after the
  request has been processed. A plain `drop` only suppresses the response and leaves the
  connection open. Silent responses (listen-only mode and requests that produce no response)
  never close the connection.
- `restart` runs after the request is processed and the response is built, and does not replace the
  response; it still happens when that response is dropped or delayed. It resets configured points
  to their configured kinds and initial values (undoing client writes), restarts ramp and sine timelines, resets lags to their `initial` value,
  resets all machines to their initial states, variables and timers and runs the initial `on-enter`
  (state registers included), and re-arms fault schedules from the restart time. The restart kind
  keeps its own schedule progress, so a scheduled restart advances through its steps and stops
  instead of re-arming itself forever. It does not clear FC 08/FC 11 diagnostic counters, the
  FC 12 event log, file records, FIFO contents, listen-only mode, or the RNG seed.
- `sensor-disconnect` replaces a successful FC 02 (discrete inputs) or FC 04 (input registers) read
  with exception 04 (slave device failure). Other function codes and error responses are
  unaffected, the connection stays open, and the request is still processed normally. If a generic
  `exception` fault also fires, its configured code wins. Scheduling it, as above, models a
  persistent sensor outage.
- Fault kinds combine: a fired exception replaces the response; `pdu` or `checksum` corruption then
  still applies to that injected exception, while `register` corruption leaves non-register
  response shapes unchanged. `drop` (including `disconnect`) suppresses the response the other
  kinds produced. A `sensor-disconnect` read also yields to a fired generic `exception` code.
- Faults do not change how the request itself is processed: writes apply, and counters and ramp
  timing update before the response is built. `delay`, `drop`, `exception`, `corrupt` and
  `disconnect` then alter only that response. `restart` additionally resets device state, and
  `sensor-disconnect` changes successful sensor reads by design. An exception replaces the response
  with `[FC|0x80][code]`. A sent injected exception is counted like a real exception and not as an
  FC11 successful completion; a suppressed injected exception counts only as no response. Real
  exceptions keep their accounting even when dropped, and busy/NAK counts use the injected code
  before any corrupt flip.
- `pdu` and `register` corruption keep the response length and leave the transport checksum valid:
  the transport recomputes CRC (RTU), LRC (ASCII), or MBAP length (TCP), so receivers see a
  structurally valid frame with wrong data, never a CRC/LRC error and no FC08 `0x000C`
  communication-error count. `checksum` corruption is the opposite: the frame reaches the wire with
  a deliberately wrong CRC/LRC. See [modbus-support.md](modbus-support.md) for the exception codes
  this simulator otherwise emits.
- Use `(seed N)` in `(server ...)` to repeat the exact same pattern of `percent`-based faults on
  every run. Schedules are deterministic and do not use the random dice.

## `(file ...)` (File Records, FC 20/21)

Defines 16-bit register records that a client can read with Modbus function code 20 (Read File
Record, 0x14) and, in this simulator, write with Modbus function code 21 (Write File Record,
0x15). File records are separate from the coil/discrete/input/holding tables. Only the ranges
configured here exist and are writable/readable (defaults to none): FC 20/21 requests targeting an
undefined file or any undefined record return exception 02. If no file records are configured, any
FC 20 read or FC 21 write is unavailable (exception 02). FC 21 updates in-memory values (readable
back via FC 20) until the simulator restarts; it does not modify the config file and does not
create new files/records.

Shape:

```scheme
(file FILE_NO
  (record REC_NO VALUE ...)
  (record REC_NO VALUE ...))
```

- `FILE_NO` is the file number, 1-65535 (`0x0001-0xFFFF`). A device can have several `(file ...)`
  entries (and several `(record ...)` entries inside a file).
- `REC_NO` is the starting record number, 0-9999 (`0x270F`).
- Each `VALUE` is a 16-bit register value, 0-65535. A record defines that many consecutive
  registers starting at `REC_NO` (one register per value, in order), and may contain at most 126
  values (`DEVICE_MAX_RECORD_VALUES`).
- Record ranges in the same file must not overlap. Record ranges must fit within record 9999.

```scheme
(file 1
  (record 0 #x1234 #x5678)   ; file 1, records 0-1
  (record 10 100 200 300))   ; file 1, records 10-12
(file 4
  (record 2 7))
```

A client request may read or write any contiguous range that is fully defined by configuration;
requesting an undefined file or any undefined record in the range returns exception 02 (illegal data
address), like undefined table addresses. Multiple sub-requests in one FC 20/21 request are
supported and answered in order (FC 21 echoes the request on success); the request is rejected as a
whole if any sub-request is invalid or unavailable/unwritable, or (for FC 20) if the response would
not fit in a Modbus PDU. FC 21 is all-or-nothing: no records are modified if any sub-request fails
validation. See [modbus-support.md](modbus-support.md) for the exact request/response format,
semantics, and exception conditions. Config is limited to 16 files per device and 64 record ranges
per file.

## `(fifo ...)` (Read FIFO Queue, FC 24)

Defines a FIFO/queue that a client can read with Modbus function code 24 (Read FIFO Queue, 0x18).
FIFO queues are separate from the coil/discrete/input/holding tables and file records. Only the
FIFO pointer addresses configured here exist (defaults to none): an FC 24 request for an undefined
FIFO pointer returns exception 02. If no FIFO queues are configured, FC 24 is unavailable
(exception 02).

Shape:

```scheme
(fifo ADDRESS (values VALUE ...))
```

- `ADDRESS` is the FIFO pointer address, 0-65535. Each `(fifo ...)` must use a different address.
- `VALUE` is a 16-bit register value, 0-65535. Values are returned by FC 24 in the order listed.
  A FIFO may contain 0 to 31 values (an empty queue is `(fifo ADDRESS (values))`).

```scheme
(fifo 100 (values #x1234 #x5678 42))
(fifo 200 (values))                      ; empty queue: FIFO Count 0
```

FC 24 reads are **non-destructive**: reading a FIFO does not change its contents or count, so
repeated reads return the same values. FIFO contents are config data (fixed for the run; not
writable via Modbus and not modified by the simulator). The response `FIFO Count` is the number of
configured values and is always at most 31. Config is limited to 16 FIFO queues per device. See
[modbus-support.md](modbus-support.md) for the exact request/response format and exception
conditions.

## `(function-code ...)` (user-defined function codes)

Defines how a device answers a function code that mogbus does not implement, such as the vendor
codes of commercial PLCs and meters. Without a definition, those codes get exception 01 (illegal
function). The Modbus specification reserves 65-72 and 100-110 for user-defined codes, but any code
1-127 that mogbus does not implement is allowed, because real products use others too.

```scheme
(function-code CODE
  (name "TEXT")                  ; optional, shown by --check
  (on "HEX PREFIX" ACTION)       ; repeatable, up to 16 cases
  ...)

(function-code CODE ACTION)      ; a bare ACTION is (on "" ACTION)
```

- `CODE` is 1-127 and must not be a code mogbus implements (01-08, 0B, 0C, 0F, 10, 11, 14-18 and
  2B). At most 8 codes per device, each defined once.
- `(name "TEXT")` is up to 63 characters and appears in `--check` and `mogbus_device_info()`.
- A request is matched against the cases in the order written. A case matches when the request
  data (the bytes after the function code) starts with its prefix; the first match answers. `""`
  matches every request. The prefix is up to 16 bytes.
- A request that matches no case gets exception 03 (illegal data value).
- Hex strings (`"01 0A FF"`) take two digits per byte, upper or lower case; spaces between bytes
  are allowed and `""` is the empty string.

| Action | Response data (after the function code) |
|---|---|
| `(reply "HEX")` | The bytes given, up to 252 |
| `(echo)` | The request data, unchanged |
| `(exception N)` | Exception response with code `N`, 1-255 |
| `(registers SPACE ADDRESS COUNT)` | A byte count and the current values of the range, laid out like FC 01-04 (bits packed least significant first for `coil` and `discrete`, big-endian 16-bit words for `input` and `holding`). `SPACE` is `coil`, `discrete`, `input` or `holding`; `COUNT` is 1-125 registers or 1-2000 bits. If any address in the range is undefined the answer is exception 02 |
| `(no-response)` | Nothing is sent |

```scheme
(device
  (unit-id 1)
  (holding 0 (count 2) (static 7))
  (function-code #x41
    (name "Vendor info")
    (on "01" (reply "04 56 45 4E 44"))   ; request 41 01 -> 41 04 56 45 4E 44
    (on "02" (registers holding 0 2))    ; request 41 02 -> 41 04 00 07 00 07
    (on "03" (echo)))
  (function-code #x64
    (on "7E" (no-response))
    (exception 4)))                      ; any other request -> exception 04
```

User-defined codes go through the same processing as the built-in ones: the device tick, the
diagnostic counters of FC 08/11/12, [metrics](observability.md), listen-only mode and
[faults](#fault-) (a `delay`, `drop`, `exception`, `corrupt` or `restart` fault applies to a
user-defined request like any other). A `sensor-disconnect` fault changes only FC 02 and 04 reads,
not user-defined codes. See [modbus-support.md](modbus-support.md#function-codes).

Codes are config-only: the C API cannot add them yet, but `mogbus_device_info()` lists them.

## Serial communication status and event log (FC 11/12)

```scheme
(com-status N)      ; N is 0 (ready) or 65535 (busy); default 0
(com-event N)       ; one event byte, 0-255; repeat up to 64 times; default empty
```

```scheme
(device (unit-id 1)
  (com-status 0)
  (com-event 128)
  (com-event 255)
  (holding 0))
```

`com-status` is the status field returned by both FC 11 and FC 12. Any value other than `0` or
`65535` is rejected by `--check` and at startup.

Each `com-event` supplies one byte of the FC 12 Events field, in the order written. Values must be
0-255 and at most 64 entries are allowed; a 65th entry is rejected. FC 12 Byte Count is
`6 + number of event bytes`, so an empty log gives Byte Count `6` and an 8-byte PDU, and 64 events
give Byte Count `70`. The simulator reports the configured bytes verbatim and does not interpret
serial-line event nibbles (it does not split an event byte into transmitter/receiver status or
decode its meaning). The log is cleared by FC 08 Restart Communications Option
with data `0xFF00`; data `0x0000` leaves it unchanged.

## Error messages

`mogbus --check FILE` reads the file, reports the first problem and exits. Messages say what
is wrong and show the entry. Common ones:

| Message | Meaning |
|---|---|
| `syntax error: missing close paren` | A `(` has no matching `)`. Check the end of the file and the entry before the reported text |
| `unknown option 'statik'. Allowed: ...` | Typo. The message lists the valid names |
| `address must be between 0 and 65535` | The address is out of range |
| `address is already used by another point` | Two points overlap. Remember 32-bit points use two addresses, 64-bit points four and strings `length` |
| `expected a number` / `expected a whole number` | A value has the wrong form, for example text in quotes |
| `missing required option (to ...)` | A ramp needs `from` and `to` |
| `string points can only be (static "TEXT")` | Strings are static; remove the ramp, sine or lag |
| `string points cannot have (noise ...) or (degradation ...)` | Strings are static text, there is no value to disturb |
| `missing required option (length ...)` | A `(type string)` point needs `(length N)`, N registers |
| `(length ...): must be between 1 and 123` | A string is 1-123 registers (two characters each) |
| `text is longer than N bytes (2 per register, length M)` | The `(static "TEXT")` does not fit its registers; raise `length` |
| `string text can only use printable ASCII (0x20-0x7E)` | Use plain ASCII characters in a string's static text |
| `(length ...) and (padding ...) only apply to (type string)` | Remove them from numeric points |
| `strings have no word order: remove (word-order ...)` | Only `byte-order` applies to strings |
| `(byte-order ...)`: `expected one of: big, little` | The only byte orders are `big` and `little` |
| `coils and discrete inputs are single bits: remove (byte-order ...)` | Byte order is for register points |
| `(access ...)`: `expected one of: read-write, read-only` | The access is `read-write` or `read-only` |
| `input registers are always read-only for clients: remove (access ...)` / `discrete inputs are always ...` | Only `coil` and `holding` points take `(access ...)`; remove it |
| `machine cannot drive a string point` / `a string point has no number to read` / `state register cannot name a string point` / `on-write cannot watch a string point` | Machines work on numbers; use a numeric point |
| `coils and discrete inputs can only be (static on/off)` | No ramps and no `type` for bit tables |
| `a point can have only one of (static ...), (ramp ...), (sine ...) and (lag ...)` | Choose one |
| `(noise ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to disturb` | Noise modifies a behaviour; add one |
| `(degradation ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to age` | Degradation modifies a behaviour; add one |
| `degradation needs at least one of drift-per-hour, noise-growth-per-hour, stuck-after-ms and fail-after-ms` | `after-ms` alone has no effect; add an effect (a drift of 0 is none) |
| `noise-growth-per-hour must be 0 or more` | Noise cannot shrink; use a positive growth |
| `coils and discrete inputs only support stuck-after-ms and fail-after-ms` | Drift and noise growth are for registers |
| `degradation numbers must be finite` | No NaN or infinity |
| `fail-after-ms`, `stuck-after-ms` or `after-ms` out of range | Times are whole milliseconds, 0-4294967295 |
| `lag target holding 9 is not the first address of a configured point` | A lag follows a configured point; declare it or fix the address |
| `a lag cannot follow itself` | Point the `target` at another point |
| `ramp step must be greater than 0` | `step` is always positive; use `from` and `to` for the direction |
| `ramp range is too large for the step` | The ramp would need 2^63 steps or more; increase `step` or narrow `from`/`to` |
| `the file has more than one (server ...); only one is allowed` | Keep a single `(server ...)` block |
| `server: parity is not valid in tcp mode` | The entry is not allowed by the selected mode; move it to a mode that supports it or remove it |
| `server: broadcast-execute is not valid in ascii-pty mode (ASCII never executes unit-0 broadcasts)` | ASCII never executes broadcasts; remove the entry |
| `server: (path ...) is required in rtu-device and ascii-device modes` | Add `(path "PATH")` |
| `devices 'A' and 'B' both use unit-id N` | Give each device its own unit id |
| `device 'A': missing (unit-id N); every device must set its Modbus unit id` | Add `(unit-id N)` to the device |
| `device 'A': unit-id is given more than once` | Keep a single `(unit-id N)` entry |
| `only one is allowed per kind` | A fault kind may be defined only once per device or per state; combine different kinds instead |
| `missing required option (code ...)` | `(kind exception)` needs `(code N)` |
| `code is only valid for kind exception` | Remove `(code ...)` from other fault kinds |
| `delay-ms is only valid for kind delay` | Remove `(delay-ms ...)` from other fault kinds |
| `mode is only valid for kind corrupt` | Remove `(mode ...)` from other fault kinds |
| `corrupt mode checksum is not valid in tcp mode (MBAP frames have no checksum)` | MBAP/TCP has no checksum; use `pdu`/`register` or a serial/RTU transport |
| `percent cannot be combined with schedule` | Choose a fixed percent chance or a schedule, not both |
| `schedule has more than 8 steps` | Shorten the schedule |
| `schedule needs at least one (step ...)` | Add a step |
| `expected (step (after-ms N) (count N))` | Each schedule entry must be a `(step ...)` form |
| `schedule step: missing required option (after-ms ...)` | Every step needs both `after-ms` and `count` |
| `(after-ms ...): must be between 0 and 86400000` | Fix the step delay |
| `(count ...): must be between 1 and 1000000` | Fix the number of requests per step |
| `code 9 is reserved` | Use one of 1-8, 10 or 11 |
| `(fault (kind disconnect)) is not valid in ... mode` | Use `tcp`, `rtu-tcp` or `ascii-tcp` (not `udp`, `ascii-udp` or `rtu-udp`), or remove the fault; this also applies to state overlays |
| `state-register must immediately follow` | Put the optional state register directly after `(initial STATE)` |
| `state register cannot name a point driven by a machine` | Use an unused static holding or input point; state registers are machine-owned |
| `on-write must follow states and precede transitions` | Keep on-write handlers after all states and before the first transition |
| `on-write is given more than once for the same point` | Combine the statements into one on-write handler for that point |
| `on-write cannot watch a machine-driven point` | Watch a client-writable point instead of a state register or machine-driven point |
| `function code N (0xNN) is implemented by mogbus and cannot be redefined` | Pick a code that is not one of 01-08, 0B, 0C, 0F, 10, 11, 14-18, 2B |
| `expected (function-code CODE ...) with CODE 1-127` | `CODE` must be a whole number from 1 to 127 |
| `needs at least one (on "HEX" ACTION) or ACTION` | A function code needs an action |
| `function code is already defined on this device` | Define each code once per device; put several cases in one `(function-code ...)` |
| `too many user-defined function codes on one device (max 8)` | Remove a code or move it to another device |
| `too many cases (max 16)` | Merge or remove `(on ...)` cases |
| `"..." is not a list of hex bytes (two digits ...)` | A `reply` or `on` string has an odd digit count or a non-hex character |
| `too many bytes (max 252)` / `(max 16)` | A `reply` holds at most 252 bytes, an `on` prefix 16 |
| `expected (reply "HEX"), (echo), (exception N), (registers SPACE ADDRESS COUNT) or (no-response)` | The action is misspelled or has the wrong number of arguments |
| `the space must be coil, discrete, input or holding` | Fix the space name in `(registers ...)` |
| `expected ADDRESS 0-65535 and COUNT 1-125 that stay below 65536` | `COUNT` is at most 125 registers (2000 for `coil` and `discrete`) and the range must end by address 65535 |
| `unknown top-level form` | Only `(server ...)` and `(device ...)` are allowed at the top |

## A complete example

```scheme
; boiler.scm
(server (mode tcp) (port 1502) (seed 1))

(device
  (name "Boiler controller")
  (unit-id 3)
  (note "Temperatures in 0.1 degrees C")

  (input 0 (type s16) (ramp (from 600) (to 850) (step 5) (every-ms 2000) (mode bounce)))
  (input 1 (type float32) (static 1.8))          ; pressure, bar (addresses 1 and 2)

  (holding 0 (type s16) (static 750))            ; setpoint, writable
  (coil 0 (static on))                           ; burner enable, writable
  (discrete 0 (static off))                      ; flame failure alarm

  (fault (kind delay) (delay-ms 400) (percent 10)))
```
