; unreliable-meter.scm - meter that sometimes does not answer at all.
;
; About 20% of responses are dropped: the client sees a timeout.
; Use it to test retry logic and "device offline" handling.
;
; Input register 0-1: energy counter (float32, kWh, counts up).

(server
  (port 1502)
  (seed 11))

(device
  (name "Unreliable energy meter")
  (unit-id 1)
  (fault (kind drop) (percent 20))

  (input 0 (type float32) (ramp (from 0) (to 100000) (step 0.25) (every-ms 1000) (mode wrap))))
