; motor.scm - a motor driven by a scripted state machine.
; See docs/user/config/machines/machine.md.
;
; Bits:
;   coil 0     run command (client writes this)
;   coil 1     fault reset (client writes this)
;   discrete 0 running (machine writes this)
; Registers:
;   input 0    current speed (machine writes this)
;   holding 0  speed limit (client writes this)

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
