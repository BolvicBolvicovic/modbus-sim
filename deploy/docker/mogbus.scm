; Container default config: Modbus TCP on every interface, port 1502.
;
; The built-in default binds 127.0.0.1, which a published container port cannot reach, so the
; container binds 0.0.0.0. Publish the port to loopback (-p 127.0.0.1:1502:1502) unless remote
; clients are meant to connect. The first device's unit id is what `mogbus --health` probes.
; The HTTP endpoint on 9102 serves /healthz, /readyz and /metrics (Prometheus).
;
; Mount your own file over /etc/mogbus/mogbus.scm, or pass another path as the container command.

(server
  (mode tcp)
  (port 1502)
  (bind "0.0.0.0")
  (idle-timeout-ms 60000)
  (http (port 9102) (bind "0.0.0.0")))

(device
  (name "Container example")
  (unit-id 1)

  (holding 0 (static 42))
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap))))
