; minimal.scm - the smallest useful device.
;
; Run it:   ./mogbus profiles/minimal.scm
; Read it:  holding register 0 always returns 42.
;           holding register 1 counts 0, 10, 20 ... 100 and starts again, one step per second.

(server
  (mode tcp)
  (port 1502))

(device
  (name "Minimal example")
  (unit-id 1)

  (holding 0 (static 42))
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap))))
