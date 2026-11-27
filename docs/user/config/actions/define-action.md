# `(define-action ...)`

Defines a named point action once, to be used by many points.

## Syntax

```scheme
(define-action NAME
  (param NAME LITERAL) ...      ; zero or more, first
  (var NAME LITERAL) ...        ; zero or more, after the params
  STATEMENT ...)                ; one or more
```

## Default

none

## Valid in

- the top level of the file, before the first point that uses it

## Behaviour

`NAME` follows the machine naming rules (lowercase letters, digits and `-`, starting with a letter, at
most 63 characters), must be unique in the file and must not be a statement or expression keyword,
`var`, `param` or `self`. A file holds at most 64 definitions, shared by all its devices.

Items come in the order params, vars, statements. `(var NAME LITERAL)` is as in a
[machine](../machines/var.md); the params and vars together are limited to 16. The statements are the
[machine statements](../machines/statements.md) and may use the [expressions](../machines/expressions.md),
the point accessors and [self](self.md).

The definition is only checked for its shape. Its statements are compiled at each use, because `self`
and the points it names differ per use; an error in them is reported at the first use, naming the device
and the point. Each use is an independent instance: its params can be [overridden](param.md) and its
variables are its own. A restart of the device sets the variables back to their initial values and keeps
the current params.

## Errors

- `define-action: bad action name 'NAME' (lowercase letters, digits and -, not a statement, expression, var, param or self)`: choose another name.
- `define-action 'NAME' is defined more than once`: names are unique.
- `too many define-action forms (max 64)`: reuse a definition or write the body inline.
- `an action needs at least one statement`: add a `set!`, `when` or `unless`.
- `param must precede var`: write the params first.
- `expected a statement like (set! v 1); param and var must come first`: params and vars cannot follow a statement.
- `unknown action NAME`: the action is used before its `define-action`, or misspelled.

## Examples

```scheme
(define-action rolling-counter
  (param step 1)
  (param limit 65535)
  (var seen 0)
  (set! seen (+ seen 1))
  (set! self (if (>= (+ self step) limit) 0 (+ self step))))

(device (unit-id 1)
  (holding 0 (static 0) (on-read (rolling-counter (step 5) (limit 100))))
  (holding 1 (static 0) (on-read (rolling-counter))))
```

## See also

- [param](param.md)
- [on-read](on-read.md)
- [on-write](on-write.md)
