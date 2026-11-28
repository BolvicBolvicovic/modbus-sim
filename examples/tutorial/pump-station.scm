; pump-station.scm - the pump station of step 1 with live values and a process behind them.

(server (mode tcp) (port 1502) (seed 7)
  (http (port 9102) (control on)))

(device
  (name "Pump station")
  (unit-id 1)

  ; Holding registers, coil and discrete inputs: as in step 1.
  (holding 0 (type float32) (static 2.5))      ; level setpoint in metres
  (holding 2 (type u16) (static 1))            ; mode: 0 off, 1 auto, 2 manual
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))
  (coil 0)                                     ; pump run command (manual mode)
  (discrete 0)                                 ; high-level alarm, set by the alarms machine
  (discrete 1)                                 ; low-level alarm, set by the alarms machine
  (discrete 2)                                 ; pump running, set by the pump machine

  ; Tank level in metres and pump run time in seconds: the pump machine below writes them.
  (input 0 (type float32) (static 2.5))
  (input 10 (type u32) (static 0))

  ; Outflow in m3/h: 40 plus or minus 6 over a two-minute period, with some random noise.
  (input 2 (type float32)
    (sine (offset 40) (amplitude 6) (period-ms 120000))
    (noise (amplitude 0.8) (every-ms 500) (distribution gaussian)))

  ; Motor current in tenths of an ampere: heads for 12.0 A (120) while "pump running" is on
  ; and back to 0 when it is off, closing 63% of the gap every 1.5 seconds.
  (input 4 (type u16)
    (lag (target (discrete 2)) (tau-ms 1500) (gain 120))
    (noise (amplitude 2) (every-ms 250)))

  ; Wall clock, seconds since 1970.
  (input 20 (type u32) (clock unix))

  ; The pump. The tank gains 2 cm per second while the pump is stopped and loses 4 cm per
  ; second while it runs. (elapsed-ms) is the time since the machine last ran.
  (machine pump
    (initial stopped)
    (var seconds 0)

    (state stopped
      (on-enter (set! (discrete 2) #f))
      (on-tick (set! (input 0) (min 5 (+ (input 0) (/ (* 0.02 (elapsed-ms)) 1000))))))
    (state running
      (on-enter (set! (discrete 2) #t))
      (on-tick (set! (input 0) (max 0 (- (input 0) (/ (* 0.04 (elapsed-ms)) 1000))))
               (set! seconds (+ seconds (/ (elapsed-ms) 1000)))
               (set! (input 10) seconds)))

    ; Start: auto mode (holding 2 = 1) and the level above setpoint + 0.3 m, or manual mode
    ; (holding 2 = 2) and the run command coil on.
    (transition stopped running
      (when (or (and (= (holding 2) 1) (> (input 0) (+ (holding 0) 0.3)))
                (and (= (holding 2) 2) (coil 0)))))
    ; Stop: mode off, or auto mode and the level below setpoint - 0.3 m, or manual mode and
    ; the run command off.
    (transition running stopped
      (when (or (= (holding 2) 0)
                (and (= (holding 2) 1) (< (input 0) (- (holding 0) 0.3)))
                (and (= (holding 2) 2) (not (coil 0)))))))

  ; The alarms: high above 4 m, low below 1 m.
  (machine alarms
    (initial watching)
    (state watching
      (on-tick (set! (discrete 0) (> (input 0) 4))
               (set! (discrete 1) (< (input 0) 1)))))

  ; One answer in fifty arrives 800 ms late, like a busy PLC.
  (fault (kind delay) (delay-ms 800) (percent 2)))
