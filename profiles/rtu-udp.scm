; rtu-udp.scm - Modbus RTU frames carried one per UDP datagram.
;
; Run it:   ./mogbus profiles/rtu-udp.scm
; Read it:  send a UDP datagram to 127.0.0.1:1502 holding one RTU frame ([addr][PDU][CRC lo hi]),
;           e.g. read holding register 0 of unit 1: 01 03 00 00 00 01 84 0A
;           The reply comes back as one datagram to the sender's address. Bad CRC, unknown
;           unit and unit 0 (broadcast) get no reply; there is no frame-gap timing.
; Faults apply as in rtu-tcp, except disconnect (no connection).

(server
  (mode rtu-udp)
  (port 1502))

(device
  (name "RTU UDP example")
  (unit-id 1)

  (holding 0 (static 42))
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap)))

  (fault (kind delay) (delay-ms 50) (percent 10)))
