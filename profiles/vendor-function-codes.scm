; vendor-function-codes.scm - a generic vendor-style device with user-defined function codes.
;
; Register map:
;   holding 0     status word, 1 = ready
;   holding 1     serial number, high word
;   holding 2     serial number, low word
;   holding 3     setpoint (writable)
;   input 0-1     process value and supply voltage, 0.1 units
;   coil 0        output (writable)
;
; User-defined function codes (request data = the bytes after the function code):
;   0x41 "Vendor info"     data 01 -> fixed reply 04 56 45 4E 44 (byte count 4, "VEND")
;                          data 02 -> byte count + holding 0-2 (status, serial number)
;                          data 03 -> byte count + input 0-1 (live values)
;                          data 04 -> echo of the request data
;                          anything else -> exception 03
;   0x64 "Vendor service"  data 00 00 -> reply 00 01 (acknowledged)
;                          data 00 FF -> exception 04 (device failure)
;                          data 7E    -> no response at all
;                          anything else -> exception 03
;
; Try it with a client that can send raw PDUs, or see examples/interop/pymodbus_interop.py.

(server (mode tcp) (port 1502))

(device
  (name "Vendor device")
  (unit-id 1)

  (holding 0 (static 1))
  (holding 1 (static #x0012))
  (holding 2 (static #x3456))
  (holding 3 (static 500))
  (input 0 (ramp (from 200) (to 300) (step 1) (every-ms 1000) (mode bounce)))
  (input 1 (static 2400))
  (coil 0 (static off))

  (function-code #x41
    (name "Vendor info")
    (on "01" (reply "04 56 45 4E 44"))
    (on "02" (registers holding 0 3))
    (on "03" (registers input 0 2))
    (on "04" (echo)))

  (function-code #x64
    (name "Vendor service")
    (on "00 00" (reply "00 01"))
    (on "00 FF" (exception 4))
    (on "7E" (no-response))))
