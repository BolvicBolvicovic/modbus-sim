# `(machine (on-write ...))`

Runs statements right after a client writes a point.

## Syntax

```scheme
(on-write POINT STATEMENT ...)
```

## Default

none

## Valid in

- `(machine ...)`

## Behaviour

Optional, repeatable (at most 16 per machine), and must follow all states and precede transitions.
`POINT` names the first address of a configured point that is not machine-driven; at most one handler
per point is allowed in one machine. The handler runs immediately after a successful client write that
touches any register of that point, after the raw value is written and in the same request. It sees
the just-written value and may read/set variables and points like other handlers, but it does not fire
transitions or advance any machine. Failures in an on-write handler are logged with the machine's
current state. Handlers in different machines run in declaration order. `(now-ms)` is the request's
tick time and `(elapsed-ms)` is measured from the machine's last tick or state change.

## Errors

- `on-write must follow states and precede transitions`: keep on-write handlers after all states and before the first transition.
- `on-write is given more than once for the same point`: combine the statements into one on-write handler for that point.
- `on-write cannot watch a machine-driven point`: watch a client-writable point instead of a state register or machine-driven point.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (static 0))     ; command register, written by the client
  (coil 0)
  (machine m
    (initial idle)
    (state idle)
    (on-write (holding 0) (when (= (holding 0) 1) (set! (coil 0) #t)))))
```

## See also

- [statements](statements.md)
- [point ownership](points.md)
