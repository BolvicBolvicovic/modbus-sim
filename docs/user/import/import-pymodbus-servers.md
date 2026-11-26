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

- The actions `timestamp` and `reset`, and custom actions: the initial value is kept as a static
  value and a warning is written. Actions on `bits` and strings likewise.
- `invalid`: ignored. Every address without a point already answers exception 02 in mogbus.
- `type exception` (rejecting requests that split a 32-bit value) and the pymodbus HTTP interface.
- A pymodbus server answers every unit id; mogbus answers the unit id of the device only.
- Several devices in one file: one device is imported per run (`--device NAME`); to serve several
  devices, run the importer once for each and merge the `(device ...)` forms by hand.
- Bit addresses above coil 65535.

## See also

- [Importing a pymodbus simulator file](import-pymodbus.md)
- [Transports](../transports/transports.md)
