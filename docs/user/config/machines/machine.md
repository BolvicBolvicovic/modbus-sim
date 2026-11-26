# `(device (machine ...))`

Declares a scripted state machine that owns some of the device's points.

## Syntax

```scheme
(machine NAME
  (initial STATE)
  (state-register (holding ADDR))    ; optional, immediately after (initial STATE)
  (var NAME LITERAL) ...             ; zero or more, before any (state ...)
  (state NAME                        ; one or more, before any (transition ...)
    (on-enter STATEMENT ...)
    (on-tick STATEMENT ...)
    (on-exit STATEMENT ...)
    (fault (kind K) ...)) ...        ; per-state faults, one definition per kind
  (on-write POINT STATEMENT ...) ... ; after states, before transitions
  (transition FROM TO TRIGGER ...) ...)
```

## Default

none

## Valid in

- `(device ...)`, repeatable (max 8 per device)

## Behaviour

Machines are read by s7 as data at config load, compiled into flat expression/statement arrays and
copied into the device arena. At request time `device_tick` only advances them: no s7 evaluation and no
allocation.

Names use lowercase letters, digits and `-`, start with a letter, and are at most 63 characters.
Machine names are unique per device; state and variable names are unique per machine. Entries must be
ordered as shown: optional state register, variables, states, on-write handlers, then transitions.
Handlers are optional and may appear in any order within a state; each handler can appear at most once
per state.

Machines on the same device share one point table: any machine can read any configured point,
including a point driven by another machine, but a point has at most one machine writer, and machines
advance in declaration order on each tick, so earlier writes are visible to later machines in the same
tick and later writes are visible to earlier machines on the next tick.

All `(machine ...)` items are compiled while each device is read; points may be declared before or
after them. At the end of that `(device ...)`, config checks deferred point references. After all
top-level forms are read, it seeds each machine RNG from [`(seed N)`](../server/seed.md) in
`(server ...)` (mixed with the device unit id) and finalizes: it validates types and point ownership,
draws initial deadlines and runs the initial state's `on-enter` at simulator start time. With no
`(server ...)` form the seed is 0. Because seeding happens after all forms, the position of
`(server ...)` does not matter. The same seed and unit id therefore give the same random deadlines on
every run.

Limits are `MACHINE_MAX_MACHINES` 8 per device, `MACHINE_MAX_STATES` 16, `MACHINE_MAX_TRANSITIONS` 32,
`MACHINE_MAX_VARS` 16, `MACHINE_MAX_WRITE_HANDLERS` 16 per machine, `MACHINE_MAX_STATEMENTS` 128
(including nested statements), `MACHINE_MAX_EXPR_NODES` 512 and `MACHINE_MAX_FIRINGS` 64 per tick. A
machine has at most one state register.

| Entry | What it does |
|---|---|
| [`(initial STATE)`](initial.md) | The state the machine starts in |
| [`(state-register ...)`](state-register.md) | Exposes the state index in a register |
| [`(var NAME LITERAL)`](var.md) | A variable |
| [`(state NAME ...)`](state.md) | A state with handlers |
| [`(state (fault ...))`](state-fault.md) | A fault that is active in one state |
| [`(on-write POINT ...)`](on-write.md) | Runs when a client writes a point |
| [`(transition FROM TO ...)`](transition.md) | Moves between states |
| [statements](statements.md), [expressions](expressions.md), [point accessors](points.md) | What handlers can contain |

## Examples

```scheme
(device
  (unit-id 1)
  (coil     0)
  (coil     1)
  (discrete 0)
  (input    0 (type u16))
  (holding  0 (type u16) (static 1200))

  (machine motor
    (initial idle)
    (var speed 0)

    (state idle
      (on-enter (set! (discrete 0) #f) (set! speed 0) (set! (input 0) 0)))

    (state starting
      (on-tick (set! speed (min (holding 0) (+ speed (/ (* 40 (elapsed-ms)) 1000))))
               (set! (input 0) speed)))

    (state running
      (on-enter (set! (discrete 0) #t))
      (on-tick  (set! (input 0) (holding 0))))

    (state tripped
      (on-enter (set! (discrete 0) #f) (set! speed 0) (set! (input 0) 0)))

    (transition idle     starting (when (coil 0)))
    (transition starting running  (after-ms 3000))
    (transition running  idle     (when (not (coil 0))))
    (transition running  tripped  (after-ms (random 60000 600000)))
    (transition tripped  idle     (when (coil 1)))))
```

See also [`profiles/motor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/motor.scm).

## See also

- [initial](initial.md)
- [transition](transition.md)
- [point ownership](points.md)
