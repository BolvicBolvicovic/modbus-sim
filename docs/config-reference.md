# Config reference

A config file describes the simulator: which transport and server options to use, and which devices
exist. Save it with any name, for example `my-device.scm`, and run `./mogbus my-device.scm`.

## Scheme in one minute

The file format comes from Scheme, but you only need these rules:

- Everything is in parentheses: `(name value)`. Every `(` needs a matching `)`.
- `;` starts a comment that runs to the end of the line.
- Text is in double quotes: `"Power meter"`.
- Numbers are written normally: `42`, `-5`, `0.5`, `230.4`.
- On/off can be written `on`, `off`, `yes`, `no`, `true`, `false`, `#t`, `#f`, `1` or `0`.
- Spaces, tabs and new lines are all the same. Indent however you like.
- Order of entries does not matter, except that a point can only be defined once.

The file is only *read as data*. Nothing in it is run, so a typo can never do anything but
produce an error message.

## Overall shape

```scheme
(server ...)    ; optional, at most once

(device ...)    ; one or more
(device ...)
```

## `(server ...)`

`(mode NAME)` selects the transport. There are seven modes; device modes require `(path "PATH")`:

| Mode | Transport | `(path ...)` |
|---|---|---|
| `tcp` | Modbus TCP (MBAP) on a TCP listen socket, optionally wrapped in TLS | forbidden |
| `rtu-pty` | Modbus RTU on a new Linux PTY (slave path is printed at startup) | forbidden |
| `rtu-device` | Modbus RTU on an existing byte-stream device | required |
| `rtu-tcp` | Modbus RTU frames on a TCP listen socket (not MBAP) | forbidden |
| `ascii-pty` | Modbus ASCII on a new Linux PTY (slave path is printed at startup) | forbidden |
| `ascii-device` | Modbus ASCII on an existing byte-stream device | required |
| `ascii-tcp` | Modbus ASCII frames on a TCP listen socket (not MBAP) | forbidden |

Only the entries listed for the selected mode are accepted. `mode` is allowed in every mode; an entry
that is present but not allowed is rejected at load time, even if its value would be the default
(for example `(stop-bits 1)` in `tcp` mode). `path` has its own required/forbidden checks.

`bind` must be a plain IPv4 or IPv6 literal: no hostnames and no zone/scoped link-local suffix such
as `"fe80::1%eth0"` (the validator uses `inet_pton()`, which does not accept `%iface`). IPv6 listen
sockets set `IPV6_V6ONLY`, so `"::"` serves IPv6 clients only and the default `"0.0.0.0"` serves IPv4
clients only. A config file contains exactly one `(server ...)` block, so there is no way to listen on
both families at once; there is no dual-stack option.

| Entry | Meaning | Default | Allowed in |
|---|---|---|---|
| `(port N)` | TCP port, 1-65535 | `1502`, or `802` when `(tls ...)` is present | `tcp`, `rtu-tcp`, `ascii-tcp` |
| `(bind "ADDRESS")` | IPv4 or IPv6 literal address to listen on. `"0.0.0.0"` = all IPv4 interfaces, `"127.0.0.1"` = this machine over IPv4 only, `"::"` = all IPv6 interfaces, `"::1"` = this machine over IPv6 only. IPv6 sockets are IPv6-only (no dual-stack), and scoped link-local literals such as `"fe80::1%eth0"` are rejected | `"0.0.0.0"` | `tcp`, `rtu-tcp`, `ascii-tcp` |
| `(seed N)` | Seed for the random dice used by faults, 0-4294967295. Same seed gives the same sequence of faults | `0` (a fixed built-in seed) | all modes |
| `(idle-timeout-ms N)` | Close a client connection after `N` milliseconds with no I/O progress, 0-86400000. `0` disables the timeout. A connection with a delayed response pending is not considered idle | `0` (disabled) | `tcp`, `rtu-tcp`, `ascii-tcp` |
| `(baud N)` | Standard termios serial baud rate (e.g. 9600, 19200, 38400, 115200). RTU derives t3.5 timing from it; in ASCII mode it only sets the serial line/PTY speed | `0` (unknown) | `rtu-pty`, `rtu-device`, `rtu-tcp`, `ascii-pty`, `ascii-device` |
| `(frame-gap-ms N)` | Explicit inter-frame silence override in milliseconds, 1-60000. Takes precedence over baud-derived t3.5 timing. If omitted and baud is unknown, RTU falls back to 20 ms | `0` (auto) | `rtu-pty`, `rtu-device`, `rtu-tcp` |
| `(strict-t15 on\|off)` | Opt-in best-effort t1.5 discard heuristic in baud-derived auto mode. Never applied to RTU-over-TCP | `off` | `rtu-pty`, `rtu-device` |
| `(broadcast-execute on\|off)` | When on, unit-0 (broadcast) requests execute on every configured device and produce no response. Never applies to MBAP/TCP or ASCII; supplying it in an ASCII mode is a load-time error | `off` | `rtu-pty`, `rtu-device`, `rtu-tcp` |
| `(data-bits 7\|8)` | Number of tty data bits. ASCII defaults to 8; `(data-bits 7)` with `(parity even)` selects traditional 7E1 Modbus ASCII | `8` | `ascii-pty`, `ascii-device` |
| `(parity none\|even\|odd\|mark\|space)` | tty parity. `mark` and `space` require `CMSPAR` support | `none` | `rtu-pty`, `rtu-device`, `ascii-pty`, `ascii-device` |
| `(stop-bits 1\|2)` | Number of tty stop bits | `1` | `rtu-pty`, `rtu-device`, `ascii-pty`, `ascii-device` |
| `(path "PATH")` | Byte-stream device path; opened with `O_NONBLOCK` | none | `rtu-device`, `ascii-device` (required there) |
| `(tls ...)` | TLS settings for the TCP listener; see [TLS](#tls-modbus-tcp-over-tls). Rejected at load time in other modes, including `--check`, and in every mode of a no-TLS build | absent (plaintext) | `tcp` |

Examples (each is a complete `(server ...)` block; a config file may contain only one):

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

RTU frame boundaries use t3.5 when baud is known (explicit `(baud N)` or, for a tty device, the
existing baud rate); `(frame-gap-ms N)` takes precedence when set. ASCII frames are
`:` + hex `[addr][PDU][LRC]` + `CR LF`, delimited by `CR LF` rather than inter-frame silence, so
RTU timing options are not allowed in ASCII modes. A `CR` at the end of a read waits for its `LF`;
`CR`, `LF`, and space between frames are ignored without incrementing the error counter. A second
`:` before the terminator resynchronizes to that new frame, and a partial frame with no further
bytes for 1 s is dropped. Discarded or undecodable ASCII frames increment the FC 08 bus
communication error counter (`0x000C`) on every device. `(baud N)` in ASCII only changes the line
speed. ASCII tty line settings default to 8N1; select 7E1 explicitly with
`(data-bits 7) (parity even) (stop-bits 1)` when talking to a master that uses the serial-line
default. These settings never change ASCII framing. They apply only to `ascii-pty` and
`ascii-device`; `ascii-tcp` carries the same CRLF-delimited frames on a TCP byte stream and accepts
only `mode`, `port`, `bind`, `seed`, and `idle-timeout-ms`. Unit-0 broadcast frames are silently
ignored in all ASCII modes; `broadcast-execute` is rejected there at load time, including by
`--check`.

## `(device ...)`

| Entry | Meaning | Default |
|---|---|---|
| `(name "Text")` | Name shown in logs and `--check`, up to 63 characters | `"unnamed"` |
| `(unit-id N)` | Modbus unit id (slave address). TCP: 0-255. RTU/ASCII: 1-247 (`0` is broadcast, `248-255` reserved by Modbus serial spec) | `1` |
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
| `(coil ...)` `(discrete ...)` `(input ...)` `(holding ...)` | Data points, see below | - |

### Read Device Identification (FC 43)

The mandatory objects and optional regular objects use `(vendor-name "Text")`,
`(product-code "Text")`, `(major-minor-revision "Text")`, `(vendor-url "Text")`,
`(product-name "Text")`, `(model-name "Text")`, and `(user-application-name "Text")`.
Extended objects use this exact syntax and may be repeated in any order:

```scheme
(device
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
- `INDEX` is 0-65535 and `SUBINDEX` is 0-255; both accept decimal or Scheme hex such as `#x6041`.
- `HEXBYTES` is 2-476 hex digits (1-238 bytes), with upper- or lowercase digits.

Each `(NODE, INDEX, SUBINDEX)` must be unique on a device, with at most 64 objects per device.
Every configured object is readable and writable through MEI `0x0D` SDO transfers, at any byte
offset within its configured data. Undefined objects and out-of-range transfers are rejected.

```scheme
(device
  (canopen-object 5 #x6041 0 "17020000")
  (canopen-object 5 #x6041 1 "AABB"))
```

Unit ids must be different between devices. In `rtu-pty`, `rtu-device`, `rtu-tcp`, `ascii-pty`,
`ascii-device` and `ascii-tcp` modes, unit ids 0 and 248-255 are rejected at startup and by
`--check` because address 0 is broadcast and 248-255 are reserved by the Modbus serial specification.

## Data points

There are four Modbus tables. Use the one your real device uses.

| Entry | Table | Holds | Can a client write it? | Function codes |
|---|---|---|---|---|
| `coil` | Coils | on/off | yes | 01, 05, 15 |
| `discrete` | Discrete inputs | on/off | no | 02 |
| `input` | Input registers | numbers | no | 04 |
| `holding` | Holding registers | numbers | yes | 03, 06, 16 |

Shape of a point:

```scheme
(holding ADDRESS
  (type ...)          ; registers only, default u16
  (word-order ...)    ; 32-bit types only, default high-first
  (static VALUE)      ; or (ramp ...), not both. Without either, the value is 0 / off
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

Coils and discrete inputs are always on/off; do not give them a `type`.
Integer register values are rounded to the nearest whole number and limited to the range of the type.
`float32` can represent `NaN`, `+inf.0`, and `-inf.0` (useful as invalid-sensor sentinels). Finite
`float32` values outside the representable float range become `+inf.0`/`-inf.0` instead of causing
undefined behavior. Non-finite static values are accepted only for `float32`; integer and on/off
static values must be finite.

A 32-bit point occupies two addresses: `(input 4 (type u32) ...)` uses addresses 4 and 5.

### `(word-order ...)`

For 32-bit types: which of the two registers holds the high half.

- `high-first` (default): the first register is the high word. This is the usual "big-endian" layout.
- `low-first`: the first register is the low word. Some devices do this ("word swapped").

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

### What happens when a client writes

Clients can write `coil` and `holding` points. The written value is stored and returned by
later reads. If the point was a ramp, **it stops moving** and keeps the written value as a
static value until the simulator is restarted.

### `(count N)`

Repeats the point `N` times at consecutive addresses (the step is the point's width: 1 for
16-bit types, 2 for 32-bit types). All copies get the same value or ramp. A device can have at
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
  (var NAME LITERAL) ...        ; zero or more, before any (state ...)
  (state NAME                   ; one or more, before any (transition ...)
    (on-enter STATEMENT ...)
    (on-tick STATEMENT ...)
    (on-exit STATEMENT ...)) ...
  (transition FROM TO TRIGGER ...) ...)
```

Handlers are optional and may appear in any order within a state.

Names use lowercase letters, digits and `-`, start with a letter, and are at most 63 characters.
Machine names are unique per device; state and variable names are unique per machine.
`(initial STATE)` is required and must name a state. Entries must be ordered as shown: variables,
then states, then transitions. Each handler can appear at most once per state.

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
error. A machine can `set!` a static point but not a ramp point, and a point may be driven by at
most one machine. A machine-driven point stays machine-owned: a client write changes the table
value but does not freeze it, so the machine keeps driving it.

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
most 64 transitions per tick.

All `(machine ...)` items are compiled while each device is read; points may be declared before or
after them. At the end of that `(device ...)`, config checks deferred point references. After all
top-level forms are read, it seeds each machine RNG from `(seed N)` in `(server ...)` (mixed with the
device unit id) and finalizes: it validates types and point ownership, draws initial deadlines and
runs the initial state's `on-enter` at simulator start time. With no `(server ...)` form the seed is
0. Because seeding happens after all forms, the position of `(server ...)` does not matter. The same
seed and unit id therefore give the same random deadlines on every run.

Limits are `MACHINE_MAX_MACHINES` 8 per device, `MACHINE_MAX_STATES` 16,
`MACHINE_MAX_TRANSITIONS` 32, `MACHINE_MAX_VARS` 16, `MACHINE_MAX_STATEMENTS` 128 (including nested
statements), `MACHINE_MAX_EXPR_NODES` 512 and `MACHINE_MAX_FIRINGS` 64 per tick.

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

See also [`profiles/motor.scm`](../profiles/motor.scm).

## `(fault ...)`

A device can have several faults. `(fault ...)` is repeatable, but **only one definition per
kind** is allowed: a second `(fault (kind X) ...)` for the same kind is rejected at config load.
Each kind rolls independently with the chance given by its `percent`.

| Entry | Meaning | Default |
|---|---|---|
| `(kind delay)`, `(kind drop)`, `(kind exception)`, `(kind corrupt)` or `(kind disconnect)` | required. `delay` sends the response late, `drop` never sends it, `exception` replaces it with a Modbus exception, `corrupt` flips a byte of the PDU response, `disconnect` drops the response and closes the client connection | - |
| `(delay-ms N)` | for `delay` only: how late, 1-60000 ms. Required for `delay`, rejected for other kinds | - |
| `(code N)` | for `exception` only: exception code to return, one of 1-8, 10 or 11 (decimal). Required for `exception`, rejected for other kinds; 9 is reserved | - |
| `(percent N)` | chance per response, 0-100 | `100` |

```scheme
(fault (kind delay) (delay-ms 1500) (percent 30))   ; 3 in 10 answers arrive 1.5 s late
(fault (kind drop) (percent 20))                    ; 1 in 5 answers never arrives
(fault (kind exception) (code 4) (percent 100))     ; every answer becomes exception 04
(fault (kind corrupt) (percent 50))                 ; half of the answers get a flipped PDU byte
(fault (kind disconnect) (percent 10))              ; 1 in 10 answers closes the connection
```

`(kind disconnect)` is valid only for TCP-like transports (`tcp`, `rtu-tcp`, `ascii-tcp`).
It is rejected at load time in `rtu-pty`, `rtu-device`, `ascii-pty` and `ascii-device`; serial
server fds are never closed by a fault.

- Delays do not block the server: other clients are served while a response waits. A configured
  delay is capped at 60000 ms.
- A dropped response means the client sees a timeout. The write or read itself **still
  happens** on the device, like a real device whose answer got lost.
- A disconnect implies a dropped response and additionally closes the connection after the
  request has been processed. A plain `drop` only suppresses the response and leaves the
  connection open. Silent responses (listen-only mode and requests that produce no response)
  never close the connection.
- Fault kinds combine on the same response: a fired exception replaces the response, and `corrupt`
  still applies to that injected exception response. A dropped response suppresses whatever the
  other kinds produced.
- Exception and corrupt faults also affect only the response: the request is still processed
  normally (writes apply, counters and ramp timing update). Exception faults replace the whole
  response with `[FC|0x80][code]`; a sent injected exception is counted like a real exception in
  FC08 and is not an FC11 successful completion. A suppressed injected exception counts only as no
  response; real exceptions keep their exception/busy/NAK accounting even when dropped. Busy/NAK
  counts use the injected code before any corrupt flip. Corrupt faults flip `resp[1]`, typically the byte count or the
  first data byte after the function code (XOR with `0xFF`, or the function code itself for a
  one-byte response); the response length does not change. The transport computes CRC (RTU),
  LRC (ASCII), or the MBAP length (TCP) over the altered PDU, so receivers see a structurally valid
  frame carrying wrong data: it is never rejected for a CRC/LRC error and does not increment the
  FC08 `0x000C` communication-error counters. See [modbus-support.md](modbus-support.md) for the
  exception codes this simulator otherwise emits.
- Use `(seed N)` in `(server ...)` to repeat the exact same pattern of faults on every run.

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
  registers starting at `REC_NO` (one register per value, in order).
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

## Serial communication status and event log (FC 11/12)

```scheme
(com-status N)      ; N is 0 (ready) or 65535 (busy); default 0
(com-event N)       ; one event byte, 0-255; repeat up to 64 times; default empty
```

```scheme
(device
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
| `address is already used by another point` | Two points overlap. Remember 32-bit points use two addresses |
| `expected a number` / `expected a whole number` | A value has the wrong form, for example text in quotes |
| `missing required option (to ...)` | A ramp needs `from` and `to` |
| `coils and discrete inputs can only be (static on/off)` | No ramps and no `type` for bit tables |
| `a point cannot have both (static ...) and (ramp ...)` | Choose one |
| `ramp step must be greater than 0` | `step` is always positive; use `from` and `to` for the direction |
| `ramp range is too large for the step` | The ramp would need 2^63 steps or more; increase `step` or narrow `from`/`to` |
| `the file has more than one (server ...)` | Keep a single `(server ...)` block |
| `server: parity is not valid in tcp mode` | The entry is not allowed by the selected mode; move it to a mode that supports it or remove it |
| `server: broadcast-execute is not valid in ascii-pty mode (ASCII never executes unit-0 broadcasts)` | ASCII never executes broadcasts; remove the entry |
| `server: (path ...) is required in rtu-device and ascii-device modes` | Add `(path "PATH")` |
| `devices 'A' and 'B' both use unit-id N` | Give each device its own unit id |
| `only one is allowed per kind` | A fault kind may be defined only once per device; combine different kinds instead |
| `missing required option (code ...)` | `(kind exception)` needs `(code N)` |
| `code is only valid for kind exception` | Remove `(code ...)` from other fault kinds |
| `delay-ms is only valid for kind delay` | Remove `(delay-ms ...)` from other fault kinds |
| `code 9 is reserved` | Use one of 1-8, 10 or 11 |
| `(fault (kind disconnect)) is not valid in ... mode` | Use `tcp`, `rtu-tcp` or `ascii-tcp`, or remove the fault |
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
