; flow-meter.scm - liquid flow meter with a totalizer.
;
; Input registers (read-only, float32 = 2 registers, high word first):
;   0  Flow rate (m3/h)        2  Fluid temperature (C)
;   4  Totalizer (m3, 32-bit unsigned integer, 2 registers)
; Bits:
;   coil 0      reset totalizer (writable)
;   discrete 0  empty-pipe alarm

(server (mode tcp) (port 1502))

(device
  (name "Flow meter")
  (unit-id 1)

  (input 0 (type float32) (ramp (from 10) (to 50) (step 0.5) (every-ms 1000) (mode bounce)))
  (input 2 (type float32) (ramp (from 14.5) (to 16.5) (step 0.1) (every-ms 4000) (mode bounce)))
  (input 4 (type u32)     (ramp (from 0) (to 4000000000) (step 1) (every-ms 1000) (mode wrap)))

  (coil 0 (static off))
  (discrete 0 (static off)))
