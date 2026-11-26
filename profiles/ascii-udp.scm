; ascii-udp.scm - Modbus ASCII over UDP: one ':'...CR LF frame per datagram.
;
; Run it:   ./mogbus profiles/ascii-udp.scm
; Read it:  send a UDP datagram to 127.0.0.1:1502 holding exactly one ASCII frame, e.g. read
;           holding register 0 of unit 1:  :010300000001FB\r\n
;           The reply comes back as one datagram to the sender's address.
; Malformed datagrams (missing CR LF, extra bytes, two frames) get no reply. Faults apply as
; in ascii-tcp, except disconnect (no connection).

(server
  (mode ascii-udp)
  (port 1502))

(device
  (name "ASCII UDP example")
  (unit-id 1)

  (holding 0 (static 42))
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap)))

  (fault (kind delay) (delay-ms 50) (percent 10)))
