# `self`

The point an action is attached to.

## Syntax

```scheme
self
(set! self EXPR)
```

## Default

none

## Valid in

- `(on-read ...)` and `(on-write ...)` bodies, and the statements of a `(define-action ...)`

## Behaviour

`self` is an expression that reads the attached point and a `set!` target that writes it, so it is a
boolean on a coil or discrete point and a number on an input or holding point. The value is the
decoded point value, as with `(holding N)`. It is not valid in a [machine](../machines/index.md)
handler, and a string point cannot have an action.

Other points are reached with the normal accessors: `(holding N)`, `(coil N)`, `(input N)` and
`(discrete N)` read, `(set! (holding N) EXPR)` writes. A point that is read or written must be
configured at its first address and must not be a string. A `set!` target must also not be driven by a
machine; this is checked once the whole file is read. Targets may be read-only, ramp, sine or other
points: writing one freezes it like a client write. No hook, machine on-write handler or other action
runs because of such a write, so actions cannot recurse.

## Errors

- `self is only valid in point actions`: used in a machine handler.
- `point read must name the first address of a configured point`: the accessor names an undefined address or the middle of a wide point.
- `set! target must name the first address of a configured point`: the same for a `set!` target.
- `a string point has no number to read or write`: strings cannot be read or written by actions.
- `set! target ... is driven by a machine`: a machine owns the target; let the machine write it.
- `assignment value has the wrong type`: a coil takes a boolean, a register a number.

## Examples

```scheme
(device (unit-id 1)
  (discrete 0 (static #f) (on-read (set! self (not self))))   ; toggles on every read
  (holding 0 (static 0) (on-read (set! self (+ self 1)))))   ; counts reads
```

## See also

- [on-read](on-read.md)
- [on-write](on-write.md)
- [point accessors](../machines/points.md)
