; pump-station.scm - the water pumping station used by the mogbus tutorials.
;
; One tank, one pump, unit id 1. The tank fills at a steady rate and the pump drains it.
; In auto mode the pump starts above the level setpoint + 0.3 m and stops below it - 0.3 m.
;
; Holding (read/write):  0-1 level setpoint (float32, m)   2 mode (0 off, 1 auto, 2 manual)
;                        10-17 site name (string, read-only)
; Coil:                  0 pump run command (used in manual mode)
; Discrete:              0 high-level alarm  1 low-level alarm  2 pump running
; Input:                 0-1 tank level (float32, m)   2-3 outflow (float32, m3/h)
;                        4 motor current (0.1 A)       10-11 pump run seconds (u32)
;                        20-21 wall clock, unix seconds (u32)

(server (mode tcp) (port 1502) (seed 7)
  (http (port 9102)))

(device
  (name "Pump station")
  (unit-id 1)

  (holding 0 (type float32) (static 2.5))
  (holding 2 (type u16) (static 1))
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))
  (coil 0)

  (discrete 0)
  (discrete 1)
  (discrete 2)

  ; Level and run seconds belong to the machine below; a client cannot write them.
  (input 0 (type float32) (static 2.5))
  (input 2 (type float32)
    (sine (offset 40) (amplitude 6) (period-ms 120000))
    (noise (amplitude 0.8) (every-ms 500) (distribution gaussian)))
  ; The motor draws about 12 A while the pump runs, following the running bit with a 1.5 s lag.
  (input 4 (type u16)
    (lag (target (discrete 2)) (tau-ms 1500) (gain 120))
    (noise (amplitude 2) (every-ms 250)))
  (input 10 (type u32) (static 0))
  (input 20 (type u32) (clock unix))

  (machine pump
    (initial stopped)
    (var seconds 0)

    ; The level rises 2 cm per second with the pump off and falls 4 cm per second with it on.
    (state stopped
      (on-enter (set! (discrete 2) #f))
      (on-tick (set! (input 0) (min 5 (+ (input 0) (/ (* 0.02 (elapsed-ms)) 1000))))))
    (state running
      (on-enter (set! (discrete 2) #t))
      (on-tick (set! (input 0) (max 0 (- (input 0) (/ (* 0.04 (elapsed-ms)) 1000))))
               (set! seconds (+ seconds (/ (elapsed-ms) 1000)))
               (set! (input 10) seconds)))

    (transition stopped running
      (when (or (and (= (holding 2) 1) (> (input 0) (+ (holding 0) 0.3)))
                (and (= (holding 2) 2) (coil 0)))))
    (transition running stopped
      (when (or (= (holding 2) 0)
                (and (= (holding 2) 1) (< (input 0) (- (holding 0) 0.3)))
                (and (= (holding 2) 2) (not (coil 0)))))))

  (machine alarms
    (initial watching)
    (state watching
      (on-tick (set! (discrete 0) (> (input 0) 4))
               (set! (discrete 1) (< (input 0) 1)))))

  ; One answer in fifty arrives 800 ms late, like a busy PLC.
  (fault (kind delay) (delay-ms 800) (percent 2)))
