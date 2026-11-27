# `(param ...)`

A read-only, per-use value of a defined action.

## Syntax

```scheme
(param NAME LITERAL)            ; in define-action
(ACTION (NAME LITERAL) ...)     ; in on-read / on-write: overrides
```

## Default

the literal of the `(param ...)` in the definition

## Valid in

- `(define-action ...)`

## Behaviour

`LITERAL` is a finite number, `#t` or `#f`; its type is the type of the param. A use of the action
may give a new value for any param by name; a name the action does not declare, a value of the other
type or the same name twice is an error. Inside the action a param reads like a variable, and a
`set!` on it is rejected at load time.

Each use stores its params in its own slots, so one definition can serve points with different
settings. The current values survive a device restart (variables are reset, params are not), and the
library keeps them addressable by point, event and name for runtime changes.

`--check` lists every instance with its current param values.

## Errors

- `unknown param 'NAME' for action 'ACTION'`: the definition has no such param.
- `param 'NAME' takes a number` / `param 'NAME' takes a boolean`: give a value of the declared type.
- `param 'NAME' is given more than once`: give each param once.
- `a param is read-only`: use a `(var ...)` for state that the action changes.
- `param is only valid in define-action`: an inline body has variables only.
- `self is a reserved name`: pick another name.

## Examples

```scheme
(define-action ramp-up
  (param step 1)
  (param enabled #t)
  (when enabled (set! self (+ self step))))

(device (unit-id 1)
  (holding 0 (static 0) (on-read (ramp-up (step 10))))
  (holding 1 (static 0) (on-read (ramp-up (enabled #f)))))
```

## See also

- [define-action](define-action.md)
- [var](../machines/var.md)
