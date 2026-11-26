# `(device (unit-id ...))`

Sets the Modbus unit id (slave address) the device answers to.

## Syntax

```scheme
(unit-id N)
```

## Default

none (required, at most once per device)

## Valid in

- `(device ...)`

## Behaviour

In `tcp` and `udp` the range is 0-255. In RTU and ASCII modes it is 1-247: `0` is broadcast and
`248-255` are reserved by the Modbus serial specification. In `rtu-pty`, `rtu-device`, `rtu-tcp`,
`ascii-pty`, `ascii-device` and `ascii-tcp` modes unit ids 0 and 248-255 are rejected at startup and
by `--check`. (`ascii-udp` and `rtu-udp` also use 1-247.) Unit ids must be unique across devices.

## Errors

- `devices 'A' and 'B' both use unit-id N`: give each device its own unit id.
- `device 'A': missing (unit-id N); every device must set its Modbus unit id`: add `(unit-id N)` to the device.
- `device 'A': unit-id is given more than once`: keep a single `(unit-id N)` entry.

## Examples

```scheme
(server (mode tcp))
(device (unit-id 1) (holding 0 (static 1)))
(device (unit-id 2) (holding 0 (static 2)))
```

## See also

- [mode](../server/mode.md)
- [broadcast-execute](../server/broadcast-execute.md)
