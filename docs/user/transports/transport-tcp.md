# Modbus TCP (`tcp`)

Modbus TCP: MBAP-framed requests over a TCP listen socket, optionally wrapped in TLS.

## Syntax

```scheme
(server (mode tcp) (port 1502) (bind "127.0.0.1") (idle-timeout-ms 5000))
```

## Default

`port 1502` (802 with TLS), `bind "127.0.0.1"`, `idle-timeout-ms 0` (disabled)

## Valid in

- `port`
- `bind`
- `seed`
- `idle-timeout-ms`
- `tls`

## Behaviour

- A frame is a 7-byte MBAP header (transaction id, protocol id 0, length, unit id) followed by the PDU.
  The response header repeats the transaction id and unit id.
- A client may keep a connection open and send several requests; they are answered one after
  another in order. Up to 32 clients are served at once; more are refused.
- A client that sends a header with a protocol id other than 0, or an impossible length, is
  disconnected.
- `bind` accepts a plain IPv4 or IPv6 literal. The default is loopback; accepting remote clients
  needs an explicit `(bind "0.0.0.0")` or `(bind "::")`. See [bind addresses](transports.md#bind-addresses).
- MBAP has no checksum, so `(fault (kind corrupt) (mode checksum))` is rejected.
- A request for a unit id that no device has gets exception 0B. Unit 0 is an ordinary id here, never a
  broadcast.
- `idle-timeout-ms` closes a connection after that many milliseconds with no I/O progress.
  Connections waiting to send a delayed fault response are not idle.

## Errors

| Message | Meaning |
|---|---|
| `built without TLS support` | `(tls ...)` in a build without TLS |
| bind failure (exit status 2) | the port is in use or the address is not local |

## Examples

```scheme
(server (mode tcp) (port 1502) (bind "127.0.0.1") (idle-timeout-ms 5000))
(device (name "Meter") (unit-id 1) (holding 0 (static 42)))
```

```bash
./mogbus profiles/minimal.scm
./tools/modbus_probe.py holding 0 2
```

## See also

- [Modbus TCP with TLS](transport-tls.md)
- [Modbus UDP](transport-udp.md)
- [Request timing](../modbus/timing.md)
