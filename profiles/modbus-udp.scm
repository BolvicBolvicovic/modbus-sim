; modbus-udp.scm - Modbus UDP: one MBAP ADU per datagram.
;
; Run it:   ./mogbus profiles/modbus-udp.scm
; Read it:  send a UDP datagram to 127.0.0.1:1502 holding an MBAP frame, e.g. read holding
;           register 0 of unit 1: 00 01 00 00 00 06 01 03 00 00 00 01
;           The reply comes back as one datagram to the sender's address.
; Faults apply as in tcp mode, except disconnect (no connection) and corrupt mode checksum.

(server
  (mode udp)
  (port 1502))

(device
  (name "UDP example")
  (unit-id 1)

  (holding 0 (static 42))
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap)))

  (fault (kind delay) (delay-ms 50) (percent 10)))
