; heater.scm - electric heater with a thermostat setpoint.
;
; Temperatures are whole numbers in tenths of a degree (215 = 21.5 C), signed.
;
; Input registers (read-only):
;   0  Water temperature (0.1 C)  follows the setpoint with a 60 s time constant, plus noise
;   1  Ambient temperature (0.1 C) slow daily swing around 20 C
; Holding registers (read/write):
;   0  Setpoint (0.1 C)           write it and watch input 0 follow
;
; Input 0 starts at 20.0 C and climbs towards the 45.0 C setpoint; write 600 to aim at 60.0 C.

(server (mode tcp) (port 1502))

(device
  (name "Heater")
  (unit-id 1)

  (holding 0 (type s16) (static 450) (note "Setpoint (0.1 C)"))

  (input 0 (type s16)
    (lag (target (holding 0)) (tau-ms 60000) (initial 200))
    (noise (amplitude 1.5) (every-ms 1000) (distribution gaussian)))

  (input 1 (type s16)
    (sine (offset 200) (amplitude 30) (period-ms 86400000))))
