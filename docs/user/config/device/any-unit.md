# `(device (any-unit ...))`

Makes the device also answer every unit id that no other device has, like a pymodbus server with a single device.

## Syntax

```scheme
(any-unit on)
(any-unit off)
```

## Default

`off`

## Valid in

- `(device ...)`

## Behaviour

Requests are routed to the device whose `unit-id` matches exactly; only when no device has the
unit id does the `any-unit` device take the request. Its own `unit-id` stays required, unique and
range-checked (1-247 in RTU and ASCII modes), and replies carry the unit id of the request, not the
device's own. Logs show the requested unit; metrics are counted on the device.

- `tcp` and `udp`: every other unit id 0-255 is answered, so the unknown-unit exception `0B` never
  happens while such a device exists. Unit 0 and 255 are ordinary ids here.
- RTU and ASCII modes (`rtu-*`, `ascii-*`, including the TCP and UDP carriers): units 1-247 are
  answered. Broadcast (unit 0) keeps its semantics: no response, and with `broadcast-execute` it
  runs on every device exactly as before; the `any-unit` device is not an extra target. Addresses
  248-255 are reserved by the Modbus serial specification and stay unanswered.

At most one device per config can set it, even with several servers.

## Errors

- `devices 'A' and 'B' both set any-unit; only one device may answer every unit id`: keep `(any-unit on)` on one device.
- `(any-unit X): expected on or off (also yes/no, true/false, 1/0)`: use `on` or `off`.

## Examples

```scheme
(server (mode tcp))
(device (unit-id 1) (holding 0 (static 1)))
(device (unit-id 2) (any-unit on) (holding 0 (static 2)))
```

Unit 1 answers 1; units 2, 3, 100 and 255 answer 2.

## See also

- [unit-id](unit-id.md)
- [Unit ids and broadcast](../../modbus/unit-ids.md)
