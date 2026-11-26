# `(state (fault ...))`

Overlays the device faults while its state is active.

## Syntax

```scheme
(fault (kind K) ...)
```

## Default

none

## Valid in

- `(state ...)`

## Behaviour

It uses the same options and one-definition-per-kind rule as a device-level
[`(fault ...)`](../faults/fault.md), and a `(kind disconnect)` overlay is rejected in serial modes for
the same reason. A checksum-corrupt overlay is rejected in `tcp` mode even when that state is never
entered. An active overlay overrides or adds its kind; kinds not mentioned keep the device setting. If
several machines are in states that define the same kind, the earliest declared machine wins for that
kind.

An overlay changes the active fault definition and therefore restarts that kind's
[schedule](../faults/schedule.md) progress; a schedule inside an overlay starts when the overlay
becomes active.

## Errors

- `only one is allowed per kind`: a fault kind may be defined only once per state.
- `(fault (kind disconnect)) is not valid in ... mode`: the overlay obeys the same transport rules as the device fault.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (static 1))
  (machine m
    (initial ok)
    (state ok)
    (state flaky (fault (kind drop) (percent 50)))
    (transition ok flaky (after-ms 5000))))
```

## See also

- [fault](../faults/fault.md)
- [disconnect](../faults/disconnect.md)
