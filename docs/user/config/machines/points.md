# Machine point accessors and ownership

How machines read and write points, and which points a machine owns.

## Syntax

```scheme
(coil ADDR)  (discrete ADDR)  (input ADDR)  (holding ADDR)
```

## Valid in

- expressions, `set!` targets, `state-register` and `on-write`

## Behaviour

Reads go through the configured point, so bit tables yield booleans and registers yield numbers with
the point's type and word order applied. Both reads and `set!` targets must name the **first address
of a configured point**; an undefined address or a continuation address of a 32-bit point is a load
error.

A machine can `set!` a static point but not a ramp, sine or lag point or a state-register point, and a
point may be driven by at most one machine. A machine-driven point stays machine-owned: a client write
to any address of it fails with exception 02 and leaves the value unchanged, so the machine keeps
driving it. Machine/internal writes still update the point normally. Machines cannot drive a point
with [`(degradation ...)`](../behaviours/degradation.md) or a [string](../points/string.md) point.

## Examples

```scheme
(device (unit-id 1)
  (discrete 0)
  (holding 0 (static 1))
  (machine m
    (initial a)
    (state a (on-enter (set! (discrete 0) (> (holding 0) 0))))))
```

## See also

- [statements](statements.md)
- [client writes](../behaviours/client-writes.md)
