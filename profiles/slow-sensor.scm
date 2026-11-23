; slow-sensor.scm - temperature sensor that sometimes answers late.
;
; About 30% of responses are delayed by 1.5 seconds. Use it to test client timeouts and retries.
; Other clients are not blocked while a response waits.
;
; Input register 0: temperature in 0.1 C (drifts between 20.0 and 25.0 C).

(server
  (mode tcp)
  (port 1502)
  (seed 7))     ; same seed = same sequence of late answers every run

(device
  (name "Slow temperature sensor")
  (unit-id 1)
  (fault (kind delay) (delay-ms 1500) (percent 30))

  (input 0 (type s16) (ramp (from 200) (to 250) (step 1) (every-ms 2000) (mode bounce))))
