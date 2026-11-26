# `(fault (kind delay))`

Sends the response late, to test client timeouts.

## Syntax

```scheme
(fault (kind delay) (delay-ms N) [(percent N)])
```

## Default

`percent 100`

## Valid in

- `(device ...)`
- every mode

## Behaviour

`delay-ms` is required, 1-60000 ms, and rejected for other kinds. Delays do not block the server:
other clients are served while a response waits. In every UDP mode, a delayed reply is held in a
16-slot table; when it is full the response is dropped (WARN in the log).

## Errors

- `delay-ms is only valid for kind delay`: remove `(delay-ms ...)` from other fault kinds.

## Examples

```scheme
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind delay) (delay-ms 1500) (percent 30)))   ; 3 in 10 answers arrive 1.5 s late
```

## See also

- [drop](drop.md)
- [fault](fault.md)
