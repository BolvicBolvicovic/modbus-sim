# `(holding (on-write ...))`

Runs an action after a Modbus client wrote the point.

## Syntax

```scheme
(on-write (ACTION (PARAM VALUE) ...))     ; a defined action
(on-write (var NAME LITERAL) ... STATEMENT ...)   ; an inline body
```

## Default

none

## Valid in

- `(coil ...)`
- `(holding ...)`

## Behaviour

The body is as for [on-read](on-read.md). It is only valid on coils and holding registers that are
not `(access read-only)`, and a point has at most one. After a client write was applied (FC 05, 06,
15, 16, 22, and the write half of FC 23) every on-write action whose point overlaps the written range
runs once, in address order, after the host's post-write hook. `self` reads the value just stored.
Writes that were vetoed or failed, and host writes, never run actions.

If an action fails the request is answered with exception 04 but the write stays applied, as with a
failing host read hook. Writing a point from the action does not run the actions, machine on-write
handlers or hooks of the points it changes.

## Errors

- `on-write is only valid on coil and holding points`: inputs and discrete inputs cannot be written by clients.
- `on-write is not valid on a read-only point`: remove `(access read-only)` or the action.
- `on-write needs a single point (remove (count N))`: attach actions to each point separately.
- `on-write needs a body`: give a defined action or statements.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (static 5))
  (coil 0 (static #f)
    (on-write (when self
                (set! (holding 0) 0)
                (set! self #f)))))        ; a self-clearing reset coil
```

## See also

- [on-read](on-read.md)
- [machine on-write](../machines/on-write.md)
- [self](self.md)
