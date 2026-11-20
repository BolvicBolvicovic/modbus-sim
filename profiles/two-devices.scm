; two-devices.scm - two devices behind one TCP port, told apart by unit id.
;
; A gateway-style setup: the client picks the device with the Modbus "unit id".
; Unit 1 is a temperature sensor, unit 2 is a flow meter.
; An unknown unit id gets exception 0x0B (gateway target failed to respond).

(server (port 1502))

(device
  (name "Room temperature sensor")
  (unit-id 1)
  (input 0 (type s16) (ramp (from 190) (to 240) (step 1) (every-ms 2000) (mode bounce))))

(device
  (name "Water flow meter")
  (unit-id 2)
  (input 0 (type float32) (ramp (from 5) (to 25) (step 0.25) (every-ms 1000) (mode bounce))))
