# `(device (fault ...))`

Injects a failure into the responses of a device: a delay, a drop, an exception, corruption, a disconnect, a restart or a sensor outage.

## Syntax

```scheme
(fault
  (kind delay|drop|exception|corrupt|disconnect|restart|sensor-disconnect)
  [(delay-ms N)]                  ; delay only
  [(code N)]                      ; exception only
  [(mode pdu|register|checksum)]  ; corrupt only, default pdu
  [(percent N)]                   ; not allowed together with schedule
  [(schedule (step (after-ms N) (count N)) ...)])
```

## Default

`percent 100`

## Valid in

- `(device ...)`
- `(state ...)` of a [machine](../machines/state-fault.md)

## Behaviour

A device can have several faults. `(fault ...)` is repeatable, but **only one definition per kind** is
allowed: a second `(fault (kind X) ...)` for the same kind is rejected at config load. A fault fires
either by its [`percent`](percent.md) chance or by its [`schedule`](schedule.md); the two are mutually
exclusive.

| Kind | Effect |
|---|---|
| [`delay`](delay.md) | Send the response late. |
| [`drop`](drop.md) | Never send the response. |
| [`exception`](exception.md) | Replace the response with a Modbus exception. |
| [`corrupt`](corrupt.md) | Corrupt the response in the selected `mode`. |
| [`disconnect`](disconnect.md) | Drop the response and close the client connection (TCP-like transports only). |
| [`restart`](restart.md) | Restart the device after the response is built. |
| [`sensor-disconnect`](sensor-disconnect.md) | Replace successful FC 02 and FC 04 reads with exception 04. |

`delay-ms` is accepted only for `delay`, `code` only for `exception` and `mode` only for `corrupt`;
`delay`, `drop`, `exception`, `restart` and `sensor-disconnect` are valid in every mode.

**How faults combine.** Fault kinds combine: a fired exception replaces the response; `pdu` or
`checksum` corruption then still applies to that injected exception, while `register` corruption leaves
non-register response shapes unchanged. `drop` (including `disconnect`) suppresses the response the
other kinds produced. A `sensor-disconnect` read also yields to a fired generic `exception` code.

**What faults do not change.** Faults do not change how the request itself is processed: writes apply,
and counters and ramp timing update before the response is built. `delay`, `drop`, `exception`,
`corrupt` and `disconnect` then alter only that response. `restart` additionally resets device state,
and `sensor-disconnect` changes successful sensor reads by design. An exception replaces the response
with `[FC|0x80][code]`. A sent injected exception is counted like a real exception and not as an FC11
successful completion; a suppressed injected exception counts only as no response. Real exceptions
keep their accounting even when dropped, and busy/NAK counts use the injected code before any corrupt
flip.

Use [`(seed N)`](../server/seed.md) in `(server ...)` to repeat the exact same pattern of
`percent`-based faults on every run. Schedules are deterministic and do not use the random dice.

## Errors

- `only one is allowed per kind`: a fault kind may be defined only once per device or per state; combine different kinds instead.
- `code is only valid for kind exception`: remove `(code ...)` from other fault kinds.
- `delay-ms is only valid for kind delay`: remove `(delay-ms ...)` from other fault kinds.
- `mode is only valid for kind corrupt`: remove `(mode ...)` from other fault kinds.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (static 1))
  (fault (kind delay) (delay-ms 1500) (percent 30))   ; 3 in 10 answers arrive 1.5 s late
  (fault (kind drop) (percent 20))                    ; 1 in 5 answers never arrives
  (fault (kind exception) (code 4) (percent 100))     ; every answer becomes exception 04
  (fault (kind corrupt) (mode register) (percent 50)) ; half of the register reads carry a wrong value
  (fault (kind sensor-disconnect)                     ; sensor reads fail during 600 requests
    (schedule (step (after-ms 5000) (count 600))))    ; after 5 s, then recover
  (fault (kind restart)                               ; one restart on the first request after 60 s
    (schedule (step (after-ms 60000) (count 1)))))
```

## See also

- [percent](percent.md)
- [schedule](schedule.md)
- [Modbus support](../../modbus/modbus-overview.md)
