# `(machine (var ...))`

Declares a machine variable with its initial value.

## Syntax

```scheme
(var NAME LITERAL)
```

## Default

none

## Valid in

- `(machine ...)`

## Behaviour

Zero or more, before any `(state ...)`; at most 16 per machine. `LITERAL` is a number or a boolean.
Variables are read as plain symbols in [expressions](expressions.md) and assigned with
[`set!`](statements.md). A restart resets them.

## Examples

```scheme
(device (unit-id 1)
  (machine m
    (initial idle)
    (var speed 0)
    (var armed #f)
    (state idle)))
```

## See also

- [statements](statements.md)
