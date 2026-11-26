# Machine expressions

The numbers, booleans, variables, point reads and operators that expressions are made of.

## Syntax

```scheme
0  1200  0.5  #t  #f  VARIABLE  (holding ADDR)  (now-ms)  (elapsed-ms)
(+ A B)  (- A B)  (* A B)  (/ A B)  (min A B)  (max A B)
(< A B)  (<= A B)  (> A B)  (>= A B)  (= A B)
(not A)  (if COND THEN ELSE)  (and A B ...)  (or A B ...)
```

## Valid in

- handlers, `when` guards and transition `(when ...)` conditions

## Behaviour

Expressions are finite numbers, booleans, a variable symbol, a point accessor, `(now-ms)`,
`(elapsed-ms)`, exactly-two-operand `+ - * / min max < <= > >= =`, `(not A)` or `(if COND THEN ELSE)`.
`(and A B ...)` and `(or A B ...)` take at least two operands and short-circuit. `(now-ms)` is the
current monotonic time; `(elapsed-ms)` is the time since this machine last ticked or changed state. At
most 512 expression nodes per machine.

There are exactly two types: **number** and **boolean**. There is no implicit conversion: arithmetic
(`+`, `-`, `*`, `/`, `min`, `max`) and ordering comparisons (`<`, `<=`, `>`, `>=`) need numbers; `=`
needs two values of the same type; `if` conditions, guards (`when`/`unless`), `not`, `and`, `or` and
transition `(when ...)` conditions need booleans; `if` branches and `set!` need matching types.
Integer and real literals are both numbers; a value assigned to a point is rounded/clamped to that
point's configured type.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (static 1200))
  (input 0)
  (machine m
    (initial a)
    (var speed 0)
    (state a
      (on-tick (set! speed (min (holding 0) (+ speed (/ (* 40 (elapsed-ms)) 1000))))
               (set! (input 0) speed)))))
```

## See also

- [statements](statements.md)
- [transition](transition.md)
