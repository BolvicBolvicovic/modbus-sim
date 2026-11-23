; temperature-controller.scm - single-loop temperature controller.
;
; Temperatures are whole numbers in tenths of a degree (215 = 21.5 C), signed.
;
; Input registers (read-only):
;   0  Process value (0.1 C)   drifts between 18.0 and 24.0 C
;   1  Output power (%)        moves between 0 and 100
; Holding registers (read/write):
;   0  Setpoint (0.1 C)
;   1  Mode: 0 = off, 1 = auto, 2 = manual
; Bits:
;   coil 0      run enable (writable)
;   discrete 0  high-temperature alarm
;   discrete 1  sensor-fault alarm

(server (mode tcp) (port 1502))

(device
  (name "Temperature controller")
  (unit-id 1)

  (input 0 (type s16) (ramp (from 180) (to 240) (step 2) (every-ms 3000) (mode bounce)))
  (input 1 (type u16) (ramp (from 0)   (to 100) (step 5) (every-ms 2000) (mode bounce)))

  (holding 0 (type s16) (static 210))
  (holding 1 (static 1))

  (coil 0 (static on))
  (discrete 0 (static off))
  (discrete 1 (static off)))
