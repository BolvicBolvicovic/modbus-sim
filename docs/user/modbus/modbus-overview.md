# Modbus support

What the simulator speaks: which function codes it answers, over which transports, and what it leaves out.

mogbus answers 19 function codes on [ten transports](../transports/transports.md).
Everything else gets exception 01, unless a device defines the code itself with a
[user-defined function code](user-defined-function-codes.md).

## Function codes at a glance

Each code has its own page in [Function codes](function-codes.md).

| Code | Name | Table | Max per request |
|---|---|---|---|
| [01](fc01-read-coils.md) | Read Coils | coils | 2000 bits |
| [02](fc02-read-discrete-inputs.md) | Read Discrete Inputs | discrete inputs | 2000 bits |
| [03](fc03-read-holding-registers.md) | Read Holding Registers | holding registers | 125 registers |
| [04](fc04-read-input-registers.md) | Read Input Registers | input registers | 125 registers |
| [05](fc05-write-single-coil.md) | Write Single Coil | coils | 1 |
| [06](fc06-write-single-register.md) | Write Single Register | holding registers | 1 |
| [07](fc07-read-exception-status.md) | Read Exception Status | - | 1 byte (status) |
| [08](fc08-diagnostics.md) | Diagnostics | - | public sub-functions 0x0000-0x0004, 0x000A-0x0012, 0x0014 |
| [11 (0x0B)](fc0b-get-com-event-counter.md) | Get Com Event Counter | - | configurable status + event count |
| [12 (0x0C)](fc0c-get-com-event-log.md) | Get Com Event Log | - | configurable status + event/message counts + 0-64 configured event bytes |
| [15 (0x0F)](fc0f-write-multiple-coils.md) | Write Multiple Coils | coils | 1968 bits |
| [16 (0x10)](fc10-write-multiple-registers.md) | Write Multiple Registers | holding registers | 123 registers |
| [17 (0x11)](fc11-report-server-id.md) | Report Server ID | - | byte count + server ID + run indicator + 0-249 bytes additional data |
| [20 (0x14)](fc14-read-file-record.md) | Read File Record | file records | 1..N sub-requests (by PDU size) |
| [21 (0x15)](fc15-write-file-record.md) | Write File Record | file records | 1..N sub-requests (by PDU size) |
| [22 (0x16)](fc16-mask-write-register.md) | Mask Write Register | holding registers | 1 register |
| [23 (0x17)](fc17-read-write-multiple-registers.md) | Read/Write Multiple Registers | holding registers | read 1-125, write 1-121 |
| [24 (0x18)](fc18-read-fifo-queue.md) | Read FIFO Queue | FIFO queues | 0-31 register values |
| 43 (0x2B) | Encapsulated Interface Transport | see below | 1-253 bytes total response |

FC 43 carries two standardized MEI types: [0x0E Read Device Identification](fc2b-0e-read-device-identification.md)
(device identification objects) and [0x0D CANopen General Reference](fc2b-0d-canopen-general-reference.md)
(the CANopen object dictionary). All other MEI types return exception 01.

Function code numbers in this reference are written in hexadecimal (`0x0B`); the decimal value is
given where the Modbus specification and the metrics use it.

## Spec coverage

All standard public function codes are implemented: FC 1 (0x01) through FC 8 (0x08), FC 11 (0x0B),
FC 12 (0x0C), FC 15 (0x0F), FC 16 (0x10), FC 17 (0x11), FC 20 (0x14) through FC 24 (0x18), and FC 43
(0x2B). Reserved and vendor-defined codes return exception 01.

Normal validation emits exception codes 01, 02, 03 and 0B (and 04 for reads of failed degraded
points), plus the extended FC 43 / MEI 0x0D failures (`AB FF ... 0D AE/CE ...`). Codes 05 to 08 and
0A are available only through the repeatable `(fault (kind exception) (code N))` injection; 04 is
also emitted by a `sensor-disconnect` fault and by reads of a failed `(degradation ...)` point; 09 is
reserved. The other fault kinds (delay, drop, corrupt, disconnect, restart) are simulator behaviour,
not Modbus exceptions, and disconnect is allowed only on TCP-like transports. See
[Exception codes](exceptions.md) and [Faults on the wire](fault-effects.md).

Remaining gaps are simplified serial-line diagnostics and config-backed data that is not created
dynamically or persisted, plus the limits below.

## Not supported (yet)

- True UART character-level t1.5 timing on byte-stream transports. Optional `(server (strict-t15 on))`
  enables a best-effort t1.5 discard heuristic at `read()` boundary granularity for RTU byte streams
  in baud-derived auto mode; it is never applied to RTU-over-TCP, and RTU-over-UDP has no inter-frame
  timing (one frame per datagram).
- TLS for RTU-over-TCP, ASCII-over-TCP, UDP, ASCII-over-UDP and RTU-over-UDP (no DTLS);
  `(server (tls ...))` is rejected outside `mode tcp`. TLS is also build-optional.
- Certificate revocation checking (CRL/OCSP): a client certificate signed by the configured CA is
  accepted for the lifetime configured in the certificate.
- Modbus Security role authorization from x.509 role OIDs: a valid client certificate is either
  required or optional, with no per-role access control.
- Dual-stack IPv6 listening: IPv6 sockets are `IPV6_V6ONLY` and a config may contain at most one
  `(server ...)` block, so `"::"` and `"0.0.0.0"` cannot be served together; scoped link-local
  literals such as `fe80::1%eth0` are rejected.

## Related pages

- [Transports](../transports/transports.md): framing, options and limits of each transport.
- [Unit ids and broadcast](unit-ids.md), [Writes are all-or-nothing](writes.md), [Request timing](timing.md).
