# `(device (name ...))`

Sets the name shown for the device in logs and `--check`.

## Syntax

```scheme
(name "Text")
```

## Default

`"unnamed"`

## Valid in

- `(device ...)`

## Behaviour

Up to 63 characters.

## Examples

```scheme
(device (name "Boiler controller") (unit-id 3) (holding 0 (static 750)))
```

## See also

- [note](note.md)
