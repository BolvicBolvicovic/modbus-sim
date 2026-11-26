# `(server (mode ...))`

Selects the transport the server speaks: Modbus TCP, UDP, RTU or ASCII on one of several carriers.

## Syntax

```scheme
(server (mode tcp|udp|rtu-pty|rtu-device|rtu-tcp|rtu-udp|ascii-pty|ascii-device|ascii-tcp|ascii-udp))
```

## Default

`tcp`

## Valid in

- `(server ...)`

## Behaviour

`tcp` and `udp` carry MBAP frames, the serial and `rtu-*` modes carry `[addr][PDU][CRC]`, and the
`ascii-*` modes carry `:` hex frames ending in CR LF.

Network modes bind to `127.0.0.1` unless `(bind "0.0.0.0")` is given, see [ramp](ramp.md).

> **Warning:** binding to `0.0.0.0` exposes the simulator to the network.

### Choosing a mode

Pick `tcp` unless your client says otherwise.

## Errors

An unknown mode fails at load time with `unknown server mode`.

## Examples

```scheme
(server (mode tcp) (port 1502))
```

```scheme
(server (mode rtu-pty))
```

```bash
./mogbus --check server.scm
```

## See also

- [ramp](ramp.md)
- [fault delay](fault-delay.md)
