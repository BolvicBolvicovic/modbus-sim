; Device used by pymodbus_interop.py. The script adds the (server ...) line for each transport.
; Register map:
;   coils 0-15            all off, writable
;   discrete inputs 0-7   0 = on, others off
;   holding 0-9           holding 0 = 1234, others 0, writable
;   input 0-1             float32 230.5
;   file 1, records 0-2   0x1234 0x5678 0x9ABC
;   FIFO pointer 5        values 1 2 3

(device
  (name "Interop test device")
  (unit-id 1)
  (vendor-name "mogbus")
  (product-code "interop")
  (major-minor-revision "1.0")
  (coil 0 (count 16))
  (discrete 0 (static on))
  (discrete 1 (count 7))
  (holding 0 (static 1234))
  (holding 1 (count 9))
  (input 0 (type float32) (static 230.5))
  (file 1 (record 0 #x1234 #x5678 #x9ABC))
  (fifo 5 (values 1 2 3)))
