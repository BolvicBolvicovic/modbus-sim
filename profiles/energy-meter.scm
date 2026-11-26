; energy-meter.scm - revenue-style energy meter with a 64-bit counter and text registers.
;
; Shows the register layouts real meters use beyond plain float32: a 64-bit integer energy
; counter, a float64, a byte-swapped float32 (BADC) and ASCII strings. This is a generic
; example, not the register map of one specific vendor.
;
; Input registers (function code 04):
;   0-3    Total active energy (Wh)   u64, counts up, high word first
;   4-7    Active power (W)           float64, high word first
;   8-9    Voltage L1 (V)             float32, byte-swapped (BADC: bytes B A D C)
;   10-11  Current L1 (A)             float32, byte-swapped (BADC)
;   20-27  Serial number              string, 8 registers (16 bytes), NUL padded
;   28-35  Model name                 string, 8 registers (16 bytes), space padded
; Holding registers (function code 03/06/16):
;   0      CT ratio
;   1      Firmware version (0x0102), read-only: a client write gets exception 02
; Bits:
;   coil 0      reset energy counter (writable)
;
; Decode BADC by swapping the two bytes of every register, then reading a normal big-endian
; float32. See docs/user/config/points/byte-order.md for the byte-order and word-order table.

(server (mode tcp) (port 1502))

(device
  (name "Energy meter")
  (unit-id 1)

  (input 0  (type u64) (ramp (from 12345678900) (to 12345999900) (step 10) (every-ms 1000) (mode wrap)))
  (input 4  (type float64) (ramp (from 2800.5) (to 3600.25) (step 12.75) (every-ms 1500) (mode bounce)))
  (input 8  (type float32) (byte-order little)
    (ramp (from 228) (to 232) (step 0.5) (every-ms 2000) (mode bounce)))
  (input 10 (type float32) (byte-order little)
    (ramp (from 4.0) (to 6.0) (step 0.1) (every-ms 1500) (mode bounce)))
  (input 20 (type string) (length 8) (static "EM3-2026-000417"))
  (input 28 (type string) (length 8) (padding space) (static "EM-100 3P"))

  (holding 0 (static 1))
  (holding 1 (access read-only) (static #x0102))
  (coil 0 (static off)))
