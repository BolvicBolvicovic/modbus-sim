; io-module.scm - remote I/O block: 16 digital in, 16 digital out, 8 analog in, 4 analog out.
;
; Analog values are in microamps of a 4-20 mA loop (4000 = 4 mA, 20000 = 20 mA).
;
;   discrete 0-7    digital inputs, off      discrete 8-15  digital inputs, on
;   coil 0-15       digital outputs (writable)
;   input 0-7       analog inputs; channel 0 sweeps the full range, the rest are fixed
;   holding 0-3     analog outputs (writable)

(server (port 1502))

(device
  (name "Remote I/O module")
  (unit-id 1)

  (discrete 0 (count 8) (static off))
  (discrete 8 (count 8) (static on))

  (coil 0 (count 16) (static off))

  (input 0 (ramp (from 4000) (to 20000) (step 160) (every-ms 1000) (mode bounce)))
  (input 1 (count 7) (static 12000))

  (holding 0 (count 4) (static 4000)))
