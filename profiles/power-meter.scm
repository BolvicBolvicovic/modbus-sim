; power-meter.scm - generic three-phase electricity meter.
;
; A typical energy meter layout: measured values are read-only input registers,
; stored as 32-bit floats (2 registers each). Settings are holding registers.
; This is a generic example, not the register map of one specific vendor.
;
; Measured values (function code 04, float32, high word first):
;   0  Voltage L1 (V)      2  Voltage L2 (V)      4  Voltage L3 (V)
;   6  Current L1 (A)      8  Current L2 (A)     10  Current L3 (A)
;  12  Active power (W)   14  Frequency (Hz)     16  Power factor
;  18  Energy (kWh, counts up)
; Settings (function code 03/06/16):
;   0  CT ratio            1  Demand period (minutes)
; Bits:
;   coil 0      reset energy counter (writable)
;   discrete 0  phase-loss alarm

(server (port 1502))

(device
  (name "Three-phase power meter")
  (unit-id 1)

  (input 0  (type float32) (ramp (from 228) (to 232) (step 0.5) (every-ms 2000) (mode bounce)))
  (input 2  (type float32) (ramp (from 229) (to 233) (step 0.5) (every-ms 2300) (mode bounce)))
  (input 4  (type float32) (ramp (from 227) (to 231) (step 0.5) (every-ms 2600) (mode bounce)))

  (input 6  (type float32) (ramp (from 4.0) (to 6.0) (step 0.1) (every-ms 1500) (mode bounce)))
  (input 8  (type float32) (ramp (from 4.2) (to 6.4) (step 0.1) (every-ms 1700) (mode bounce)))
  (input 10 (type float32) (ramp (from 3.8) (to 5.8) (step 0.1) (every-ms 1900) (mode bounce)))

  (input 12 (type float32) (ramp (from 2800) (to 3600) (step 50) (every-ms 1500) (mode bounce)))
  (input 14 (type float32) (ramp (from 49.95) (to 50.05) (step 0.01) (every-ms 3000) (mode bounce)))
  (input 16 (type float32) (static 0.97))
  (input 18 (type float32) (ramp (from 1000) (to 100000) (step 0.5) (every-ms 1000) (mode wrap)))

  (holding 0 (static 1))
  (holding 1 (static 15))

  (coil 0 (static off))
  (discrete 0 (static off)))
