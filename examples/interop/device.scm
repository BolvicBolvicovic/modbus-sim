; Device used by pymodbus_interop.py. The script adds the (server ...) line for each transport.
; Register map:
;   coils 0-15            all off, writable
;   discrete inputs 0-7   0 = on, others off
;   holding 0-9           holding 0 = 1234, others 0, writable
;   input 0-1             float32 230.5
;   holding 100-103       u64 0x0123456789ABCDEF
;   holding 104-107       s64 -123456789012
;   holding 108-111       float64 -1234.5678
;   holding 112-115       u64 0x0102030405060708, low-first (register order reversed)
;   holding 116-117       float32 230.5, byte-order little (BADC)
;   holding 118-119       float32 230.5, byte-order little + low-first (DCBA)
;   holding 120-123       s64 -2, byte-order little
;   holding 124-131       string "mogbus-sim", NUL padded, 8 registers
;   holding 140-143       string, 4 registers, empty, writable
;   input 10-13           string "ABCDEFGH", byte-order little
;   file 1, records 0-2   0x1234 0x5678 0x9ABC
;   FIFO pointer 5        values 1 2 3
;   function code 0x41    selector 01: fixed reply 04 DE AD BE EF; selector 02: holding 0-1 as
;                         byte count + 2 registers

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
  (holding 100 (type u64) (static #x0123456789ABCDEF))
  (holding 104 (type s64) (static -123456789012))
  (holding 108 (type float64) (static -1234.5678))
  (holding 112 (type u64) (word-order low-first) (static #x0102030405060708))
  (holding 116 (type float32) (byte-order little) (static 230.5))
  (holding 118 (type float32) (byte-order little) (word-order low-first) (static 230.5))
  (holding 120 (type s64) (byte-order little) (static -2))
  (holding 124 (type string) (length 8) (static "mogbus-sim"))
  (holding 140 (type string) (length 4))
  (input 10 (type string) (length 4) (byte-order little) (static "ABCDEFGH"))
  (file 1 (record 0 #x1234 #x5678 #x9ABC))
  (fifo 5 (values 1 2 3))
  (function-code #x41
    (name "Vendor test")
    (on "01" (reply "04 DE AD BE EF"))
    (on "02" (registers holding 0 2))))
