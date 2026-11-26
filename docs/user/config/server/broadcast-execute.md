# `(server (broadcast-execute ...))`

Makes unit-0 (broadcast) requests execute on every device, still without a response.

## Syntax

```scheme
(broadcast-execute on|off)
```

## Default

`off`

## Valid in

- `rtu-pty`
- `rtu-device`
- `rtu-tcp`
- `rtu-udp`

## Behaviour

When on, a unit-0 request executes on every configured device and produces no response. Requests that
produce no response do not consume [fault schedule](../faults/schedule.md) steps. It never applies to
MBAP/TCP or ASCII; supplying it in an ASCII or MBAP mode is a load-time error.

## Errors

- `server: broadcast-execute is not valid in ascii-pty mode (ASCII never executes unit-0 broadcasts)`: ASCII never executes broadcasts; remove the entry.

## Examples

```scheme
(server (mode rtu-tcp) (broadcast-execute on))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [mode](mode.md)
- [unit-id](../device/unit-id.md)
