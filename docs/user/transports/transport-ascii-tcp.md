# Modbus ASCII over TCP (`ascii-tcp`)

Raw Modbus ASCII frames on a TCP listen socket: `:` + hex + `CR LF`, no MBAP header and no TLS.

## Syntax

```scheme
(server (mode ascii-tcp) (port 1502) (bind "0.0.0.0") (idle-timeout-ms 5000))
```

## Default

`port 1502`, `bind "127.0.0.1"`

## Valid in

- `port`
- `bind`
- `seed`
- `idle-timeout-ms`

## Behaviour

- `port` and `bind` select the listen socket. `bind` takes an IPv4 or IPv6 literal (IPv6 sockets are
  IPv6-only), and the default is loopback; bind to `"0.0.0.0"` or `"::"` to accept remote clients.
- Up to 32 clients are served by one poll loop, each with its own receive buffer and delayed-response
  slot. `idle-timeout-ms` closes connections with no I/O progress.
- Framing, resynchronization, error counting, unit routing and fault behaviour are those of
  [serial ASCII](transport-ascii.md): complete frames are answered as soon as `CR LF` arrives,
  multiple frames per read and split frames work, a trailing `CR` waits for the next `LF`, and a
  partial frame with no byte for 1 s is dropped and counted as one FC 08 `0x000C` error per device.
- Unlike serial ASCII, `(fault (kind disconnect))` is accepted here and closes the client connection.
- Broadcast (unit 0) is silently ignored with no PDU processing. Unit ids are 1 to 247.
- It is not Modbus TCP (no MBAP). `(baud N)` has no meaning here.

## Errors

| Message | Meaning |
|---|---|
| load error | `baud`, `frame-gap-ms`, `strict-t15`, `data-bits`, `parity`, `stop-bits`, `path`, `broadcast-execute` or `tls`; `--check` rejects them too |

## Examples

```scheme
(server (mode ascii-tcp) (port 1502) (bind "0.0.0.0") (idle-timeout-ms 5000))
(device (name "ASCII over TCP") (unit-id 1) (holding 0 (static 42)))
```

## See also

- [Modbus ASCII](transport-ascii.md)
- [Modbus ASCII over UDP](transport-ascii-udp.md)
