# Machine expressions

The numbers, booleans, variables, point reads and operators that expressions are made of.

## Syntax

```scheme
0  1200  0.5  #t  #f  VARIABLE  (holding ADDR)  (now-ms)  (elapsed-ms)
(+ A B)  (- A B)  (* A B)  (/ A B)  (min A B)  (max A B)
(mod A B)  (abs A)  (floor A)  (round A)  (truncate A)
(bit-and A B)  (bit-or A B)  (bit-xor A B)  (shift A N)  (bit-set? A N)
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

`(mod A B)` is the floored modulo of Scheme `modulo`: the result has the sign of `B` (`(mod -7 3)` is 2,
`(mod 7 -3)` is -2). `(abs A)`, `(floor A)` and `(truncate A)` have their usual meaning, and `(round A)`
rounds half to even like Scheme `round` (`(round 2.5)` is 2, `(round 3.5)` is 4).

The bit operators work on whole numbers from 0 to 2^53-1 (the range in which every integer is exact in
the 64-bit floating point numbers that expressions use):

- `(bit-and A B)`, `(bit-or A B)` and `(bit-xor A B)` combine the bits of both operands.
- `(shift A N)` is Scheme `ash`: a positive whole `N` shifts left, a negative one shifts right, and
  `|N|` is at most 53. A left shift whose result would reach 2^53 fails.
- `(bit-set? A N)` is a boolean: bit `N` (0 to 52, bit 0 is the least significant) of `A` is set. Use it on
  status words.

For example `(bit-and (holding 0) 255)` is the low byte of a 16-bit register and
`(bit-set? (holding 0) 3)` tests its bit 3.

Evaluation fails (see Errors) when `B` of `mod` is 0, when a bit operand is negative, fractional,
not a number or 2^53 or more, or when a shift or bit index is out of range. A failed expression
skips the statement that contains it; the machine keeps running.

There are exactly two types: **number** and **boolean**. There is no implicit conversion: arithmetic
(`+`, `-`, `*`, `/`, `min`, `max`, `mod`, `abs`, `floor`, `round`, `truncate`, the bit operators and `shift`) and ordering comparisons (`<`, `<=`, `>`, `>=`) need numbers; `=`
needs two values of the same type; `if` conditions, guards (`when`/`unless`), `not`, `and`, `or` and
transition `(when ...)` conditions need booleans; `if` branches and `set!` need matching types.
Integer and real literals are both numbers; a value assigned to a point is rounded/clamped to that
point's configured type.

## Errors

- `expected exactly two operands`: `mod`, the bit operators, `shift` and `bit-set?` take two operands.
- `expected exactly one operand`: `abs`, `floor`, `round` and `truncate` take one operand.
- `operands must be numbers` / `operand must be a number`: these operators accept numbers only.
- `division by zero`: `(mod A 0)` fails when evaluated, like `/`; the statement is skipped and logged.
- `integer operand out of range`: a bit operand that is not a whole number from 0 to 2^53-1, a shift of
  more than 53 places or one that reaches 2^53, or a `bit-set?` index outside 0 to 52 fails when
  evaluated; the statement is skipped and logged.

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

```scheme
(device (unit-id 1)
  (holding 0 (static 0))
  (coil 0)
  (input 0)
  (machine status
    (initial a)
    (state a
      (on-tick (set! (coil 0) (bit-set? (holding 0) 3))
               (set! (input 0) (mod (round (+ (now-ms) 0.5)) 360))))))
```

## See also

- [statements](statements.md)
- [transition](transition.md)
