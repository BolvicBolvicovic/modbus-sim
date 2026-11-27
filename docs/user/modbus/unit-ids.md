# Unit ids and broadcast

Each device answers to its own unit id, and one device can also answer every other id. This page covers how requests are routed, which ids are valid, and how broadcast works.

## Routing

Each `(device ...)` must set `(unit-id N)` and answers to that id. A client using an id no device
has gets exception 0B in the MBAP transports (`tcp`, `udp`) and no response in RTU and ASCII.

To answer every id with one device (the pymodbus "single" behaviour), give it
[`(any-unit on)`](../config/device/any-unit.md): an exact match still wins, and the any-unit device
takes all other requests, so exception 0B never happens while it exists. Replies carry the
request's unit id. RTU and ASCII still leave broadcast (0) and the reserved addresses 248-255
unanswered; in `tcp` and `udp` units 0 and 255 are answered like any other id. At most one device
can have it; the C API calls it `mogbus_device_set_any_unit`.

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
