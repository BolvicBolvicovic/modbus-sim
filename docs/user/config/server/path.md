# `(server (path ...))`

Names the existing byte-stream device that the `rtu-device` and `ascii-device` modes serve.

## Syntax

```scheme
(path "PATH")
```

## Default

none

## Valid in

- `rtu-device` (required)
- `ascii-device` (required)

## Behaviour

The path is opened with `O_NONBLOCK`. Any other mode forbids `path`; the PTY modes create their own
device and print its slave path at startup.

## Errors

- `server: (path ...) is required in rtu-device and ascii-device modes`: add `(path "PATH")`.

## Examples

```scheme
(server (mode rtu-device) (path "/dev/ttyUSB0") (baud 19200))
(device (unit-id 1) (holding 0 (static 1)))
```

## See also

- [mode](mode.md)
- [baud](baud.md)
