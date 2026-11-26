# `(note ...)`

A comment kept for humans, ignored by the program.

## Syntax

```scheme
(note "Text")
```

## Default

none

## Valid in

- `(device ...)`
- point forms such as `(holding ...)`

## Behaviour

Can be repeated.

## Examples

```scheme
(device (unit-id 3)
  (note "Temperatures in 0.1 degrees C")
  (input 0 (type s16) (static 600) (note "water temperature")))
```

## See also

- [name](name.md)
