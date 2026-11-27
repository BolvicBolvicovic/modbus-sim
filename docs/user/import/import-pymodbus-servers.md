# Servers and what is not supported

How a pymodbus `server_list` entry becomes a `(server ...)` section, and what the importer leaves out.

## Servers

| `comm` / `framer` | mogbus mode |
|---|---|
| `tcp` / `socket` | `tcp` (`host` becomes `bind`, `port` the port) |
| `tcp` / `rtu` | `rtu-tcp` |
| `tcp` / `ascii` | `ascii-tcp` |
| `udp` / `socket`, `rtu`, `ascii` | `udp`, `rtu-udp`, `ascii-udp` |
| `serial` / `rtu`, `ascii` | `rtu-device`, `ascii-device` (`port` is the path, `baudrate`, `parity` N/E/O, `stopbits`, and for ASCII `bytesize` 7 or 8) |
| `tls` / `tls` | `tcp` with `(tls (certificate ...) (private-key ...) (require-client-certificate off))` and a warning: add a `ca-certificate` to require client certificates |

Anything else gets a warning and no server section.

## Not supported

- Custom Python actions: the initial value is kept as a static value and a warning is written. Other
  actions on `bits` and strings likewise. (`timestamp` on registers is mapped to clock points and
  `reset` to a failing read, see [the reset action](import-pymodbus.md).)
- `invalid`: ignored. Every address without a point already answers exception 02 in mogbus.
- `type exception` (rejecting requests that split a 32-bit value) and the pymodbus HTTP interface.
- A pymodbus server with a `device_id` answers that unit id only, and the importer follows it; without
  one it answers every unit id and the importer writes `(any-unit on)` (`--exact-unit` omits it).
  With `--all-devices` only the assigned unit ids answer.
- Bit addresses above coil 65535.

## See also

- [Importing a pymodbus simulator file](import-pymodbus.md)
- [Transports](../transports/transports.md)
