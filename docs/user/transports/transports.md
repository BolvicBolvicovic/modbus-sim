# Transports

`(server (mode NAME))` selects how clients reach the simulator: one of ten modes, from Modbus TCP to RTU frames on a serial line. One page per transport.

A config selects exactly one mode. Every mode serves the same devices and the same
[function codes](../modbus/function-codes.md); only the framing and the transport differ.

| Mode | Transport | Server entries allowed with `mode` |
|---|---|---|
| [`tcp`](transport-tcp.md) | Modbus TCP (MBAP) over TCP (IPv4 or IPv6 literal bind), optionally [TLS-wrapped](transport-tls.md) | `port`, `bind`, `seed`, `idle-timeout-ms`, `tls` |
| [`rtu-pty`](transport-rtu.md) | Modbus RTU over a new Linux PTY (slave path printed at startup) | `seed`, `baud`, `frame-gap-ms`, `strict-t15`, `broadcast-execute`, `parity`, `stop-bits` |
| [`rtu-device`](transport-rtu.md) | Modbus RTU over an existing byte-stream device | `path`, plus the `rtu-pty` entries |
| [`rtu-tcp`](transport-rtu-tcp.md) | Modbus RTU frames over a TCP listen socket (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `idle-timeout-ms`, `baud`, `frame-gap-ms`, `broadcast-execute` |
| [`ascii-pty`](transport-ascii.md) | Modbus ASCII over a new Linux PTY (slave path printed at startup) | `seed`, `baud`, `data-bits`, `parity`, `stop-bits` |
| [`ascii-device`](transport-ascii.md) | Modbus ASCII over an existing byte-stream device | `path`, plus the `ascii-pty` entries |
| [`ascii-tcp`](transport-ascii-tcp.md) | Raw Modbus ASCII frames over a TCP listen socket (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `idle-timeout-ms` |
| [`udp`](transport-udp.md) | Modbus UDP: one MBAP ADU per datagram (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed` |
| [`ascii-udp`](transport-ascii-udp.md) | Modbus ASCII: one `:`...CR LF frame per UDP datagram (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed` |
| [`rtu-udp`](transport-rtu-udp.md) | Modbus RTU frames, one per UDP datagram (IPv4 or IPv6 literal bind) | `port`, `bind`, `seed`, `broadcast-execute` |

`mode` itself is allowed in every mode. Device modes require `(path "PATH")`; other modes reject it.
Any other entry is rejected at load time, even when its value is the default. See the
[config reference](../config/format.md) for ranges and defaults.

## Bind addresses

TCP-family listeners (`tcp`, `rtu-tcp`, `ascii-tcp`) and the UDP modes (`udp`, `ascii-udp`,
`rtu-udp`) bind to loopback (`127.0.0.1`) by default, so they accept only local clients. Bind
explicitly to `0.0.0.0` (or `::` for IPv6) to accept remote connections.

- `bind` takes a plain IPv4 or IPv6 literal address. Scoped link-local literals such as
  `"fe80::1%eth0"` are rejected by the config validator.
- IPv6 listen sockets set `IPV6_V6ONLY`, so `"0.0.0.0"` and `"::"` listen on separate address
  families and cannot both be used. A config may contain at most one `(server ...)` block (defaults
  apply if omitted), and there is no dual-stack option.
- These rules apply to all six network modes.

## Checksums and framing at a glance

| Transport | Frame | Checksum |
|---|---|---|
| `tcp`, `udp` | 7-byte MBAP header + PDU | none |
| `rtu-*` | `[addr][PDU][CRC lo hi]` | CRC-16, low byte first |
| `ascii-*` | `:` + uppercase hex `[addr][PDU][LRC]` + `CR LF` | LRC |

MBAP has no checksum field, so `(fault (kind corrupt) (mode checksum))` is rejected in `tcp` and
`udp`; the RTU and ASCII transports carry checksums and accept it.

## Choosing a transport

- A SCADA, PLC or gateway that speaks Modbus TCP: `tcp`.
- A serial client on the same machine: a PTY mode, which creates a pseudo-terminal and prints its path.
- A real serial port: a `*-device` mode with `(path "/dev/ttyUSB0")`.
- A client that tunnels serial frames over the network: `rtu-tcp` or `ascii-tcp`.
- Datagram clients: `udp`, `rtu-udp` or `ascii-udp`.

## See also

- [Modbus support](../modbus/modbus-overview.md)
- [Config reference: server](../config/server/index.md)
