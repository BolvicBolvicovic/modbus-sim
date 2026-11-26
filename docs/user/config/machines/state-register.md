# `(machine (state-register ...))`

Exposes the current state, as its zero-based index, in a register.

## Syntax

```scheme
(state-register (holding ADDR))
(state-register (input ADDR))
```

## Default

none

## Valid in

- `(machine ...)`

## Behaviour

Optional, at most once per machine, and must be the first item after `(initial STATE)`. It names the
first address of a configured static register point (not a ramp and not driven by any machine). That
point becomes machine-owned and read-only to clients; it holds the zero-based index of the current
state as a number. It is written before the initial state's `on-enter` and after each state switch,
before the new state's `on-enter`, and is readable from machine expressions and Modbus reads.

## Errors

- `state-register must immediately follow`: put the optional state register directly after `(initial STATE)`.
- `state register cannot name a point driven by a machine`: use an unused static holding or input point; state registers are machine-owned.

## Examples

```scheme
(device (unit-id 1)
  (input 0 (type u16))
  (machine m
    (initial idle)
    (state-register (input 0))
    (state idle)
    (state busy)
    (transition idle busy (after-ms 2000))))
```

## See also

- [machine](machine.md)
- [point ownership](points.md)
