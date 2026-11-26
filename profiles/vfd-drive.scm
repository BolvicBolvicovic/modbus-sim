; vfd-drive.scm - variable frequency drive for a motor.
;
; The drive "accelerates" after start-up: output frequency and current climb and then hold.
; Units: frequency in 0.01 Hz (5000 = 50.00 Hz), current in 0.1 A, voltage in 0.1 V,
; temperature in 0.1 C.
;
; Input registers (read-only):
;   0  Status word              1  Output frequency (0.01 Hz)   2  Motor current (0.1 A)
;   3  DC bus voltage (0.1 V)   4  Drive temperature (0.1 C)
;   5-6  Energy counter (32-bit, high word first)
; Holding registers (read/write):
;   0  Control word             1  Frequency setpoint (0.01 Hz)
;   2  Fault code (read-only: a client write gets exception 02)
; Bits:
;   coil 0 run command   coil 1 fault reset
;   discrete 0 running   discrete 1 fault active

(server (mode tcp) (port 1502))

(device
  (name "Variable frequency drive")
  (unit-id 1)

  (input 0 (static 1))
  (input 1 (ramp (from 0) (to 5000) (step 250) (every-ms 500) (mode hold)))
  (input 2 (ramp (from 0) (to 120)  (step 6)   (every-ms 500) (mode hold)))
  (input 3 (static 5400))
  (input 4 (ramp (from 350) (to 520) (step 5) (every-ms 5000) (mode bounce)))
  (input 5 (type u32) (ramp (from 0) (to 4000000000) (step 10) (every-ms 1000) (mode wrap)))

  (holding 0 (static 0))
  (holding 1 (static 5000))
  (holding 2 (access read-only) (static 0))

  (coil 0 (static on))
  (coil 1 (static off))
  (discrete 0 (static on))
  (discrete 1 (static off)))
