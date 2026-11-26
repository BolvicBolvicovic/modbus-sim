# Unit ids and broadcast

Each device answers to exactly one unit id. This page covers how requests are routed, which ids are valid, and how broadcast works.

## Routing

Each `(device ...)` must set `(unit-id N)` and answers only to that id. There is no "any unit id"
mode: a client using the wrong id gets exception 0B in the MBAP transports (`tcp`, `udp`). If a
client sends 255 or 0 by default, set the device's `unit-id` to match.

## Valid ids in RTU and ASCII

In RTU and ASCII modes unit ids must be 1 to 247. Address 0 is Modbus broadcast and 248 to 255 are
reserved by the Modbus serial specification, so a device with one of these ids could not be
addressed by a normal serial request. mogbus rejects such ids when it starts in an RTU or ASCII mode,
and `--check` rejects them when it reads such a mode.

## Broadcast (unit 0)

- **RTU transports** (`rtu-pty`, `rtu-device`, `rtu-tcp`, `rtu-udp`): broadcast requests are received
  but ignored by default. With `(server (broadcast-execute on))` they execute on every configured
  device (tick, counters, writes) and still produce no response; fault delays, drops and
  disconnects do not apply because there is no response.
- **ASCII transports**: broadcast frames are received but never executed. `broadcast-execute` is
  rejected at load time.
- **MBAP** (`tcp`, `udp`): unit 0 is just a unit id and is never a broadcast.

## See also

- [Modbus RTU](../transports/transport-rtu.md)
- [Modbus ASCII](../transports/transport-ascii.md)
- [Config reference: server](../config/server/index.md)
