; multi-server.scm - several servers in one file, running together.
;
; Run it:   ./mogbus profiles/multi-server.scm
;
; Three servers, each named with (id ...). A device lists the servers that serve it with
; (servers ...), so one device can be reached over several transports at once.
;
;   plant   Modbus TCP on 127.0.0.1:1502        unit 1 (meter)
;   gateway RTU-over-TCP on 127.0.0.1:1503      unit 1 (meter) again, with RTU framing
;   legacy  Modbus UDP on 127.0.0.1:1502        unit 2 (sensor) only
;
; Unit ids only have to be unique per server. TCP and UDP use separate port spaces, so "plant"
; and "legacy" may share 1502. Both transports of unit 1 show the same registers.
;
; Holding register 0 of the meter is the setpoint (write it from either transport and read it
; back from the other). The sensor's input 0 is a ramp.

(server
  (id "plant")
  (mode tcp)
  (port 1502))

(server
  (id "gateway")
  (mode rtu-tcp)
  (port 1503))

(server
  (id "legacy")
  (mode udp)
  (port 1502))

(device
  (name "Meter")
  (unit-id 1)
  (servers "plant" "gateway")
  (holding 0 (static 100))
  (input 0 (ramp (from 0) (to 100) (step 5) (every-ms 1000) (mode wrap))))

(device
  (name "UDP sensor")
  (unit-id 2)
  (servers "legacy")
  (input 0 (ramp (from 200) (to 300) (step 10) (every-ms 1000) (mode bounce))))
