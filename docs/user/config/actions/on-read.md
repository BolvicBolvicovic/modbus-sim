# `(holding (on-read ...))`

Runs an action each time a Modbus client reads the point.

## Syntax

```scheme
(on-read (ACTION (PARAM VALUE) ...))     ; a defined action
(on-read (var NAME LITERAL) ... STATEMENT ...)   ; an inline body
```

## Default

none

## Valid in

- `(coil ...)`
- `(discrete ...)`
- `(input ...)`
- `(holding ...)`

## Behaviour

The body is either exactly one use of a [defined action](define-action.md), with optional
[param](param.md) overrides, or an inline body of optional `(var ...)` items followed by one or more
[statements](../machines/statements.md). Inline bodies have no params. At most one `on-read` per point,
and it needs a single point: `(count N)` with more than one copy is rejected, and so are string points.

When a client request reads a range, the device first checks that the range exists and runs the
host's access hook; a veto from the hook ends the request and no action runs. Then every on-read
action whose point overlaps the range runs once, in address order (a 32-bit point read through both of
its registers runs once), and the table is read afterwards, so the client sees what the actions
stored. This covers FC 01, 02, 03, 04, the read half of FC 23 and a user-defined function code with a
`registers` action. Host reads never run actions. `(now-ms)` is the tick time of the request and
`(elapsed-ms)` the time since this instance last ran, 0 on its first run.

If an action fails the request is answered with exception 04 (server device failure) and the failure is
logged once per instance and failure kind with the device unit and the point.

## Errors

- `on-read needs a single point (remove (count N))`: attach actions to each point separately.
- `on-read is not valid on a string point`: strings cannot have actions.
- `on-read needs a body`: give a defined action or statements.
- `unknown action NAME`: define it before the device.
- `is given more than once`: a point has at most one on-read.

## Examples

```scheme
(define-action counter
  (param step 1)
  (var n 0)
  (set! n (+ n step))
  (set! self n))

(device (unit-id 1)
  (holding 0 (static 0) (on-read (counter (step 5))))
  (holding 1 (static 0) (on-read (set! self (* 2 (holding 0))))))
```

## See also

- [on-write](on-write.md)
- [define-action](define-action.md)
- [self](self.md)
