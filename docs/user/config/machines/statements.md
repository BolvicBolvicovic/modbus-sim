# Machine statements

The statements a handler can contain: `set!`, `when` and `unless`.

## Syntax

```scheme
(set! TARGET EXPR)
(when EXPR STATEMENT ...)
(unless EXPR STATEMENT ...)
```

## Valid in

- `on-enter`, `on-tick`, `on-exit` and `on-write` handlers, and nested in guards

## Behaviour

`TARGET` is a variable name or a [point accessor](points.md). A guard runs its body when its condition
is true (`when`) or false (`unless`); guards can nest. The condition must be a boolean, and `set!` needs
matching types. At most 128 statements per machine, nested ones included.

## Examples

```scheme
(device (unit-id 1)
  (coil 0)
  (holding 0 (static 0))
  (machine m
    (initial a)
    (var n 0)
    (state a
      (on-tick (set! n (+ n 1))
               (when (> n 3) (set! (coil 0) #t))
               (unless (> n 3) (set! (coil 0) #f))))))
```

## See also

- [point actions](../actions/index.md), which use the same statements on a single point
- [expressions](expressions.md)
- [point ownership](points.md)
