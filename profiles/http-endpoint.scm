; http-endpoint.scm - the HTTP endpoint: health, metrics, control API and web UI.
;
; Start it, then open http://127.0.0.1:9102/ in a browser. Everything is loopback only.
;
;   curl http://127.0.0.1:9102/healthz     200 while the server loop runs
;   curl http://127.0.0.1:9102/readyz      200 while serving
;   curl http://127.0.0.1:9102/metrics     Prometheus text (requests, exceptions, faults, ...)
;   curl http://127.0.0.1:9102/api/devices/1/points?space=holding   the control API
;
; Holding registers (read/write):
;   0  Setpoint (0.1 C)      write it and watch input 0 follow in the UI Plot view
; Input registers (read-only):
;   0  Temperature (0.1 C)   follows the setpoint with a 20 s time constant
;   1  Tank level (%)        ramp 0-100 that bounces
;   2  Supply wave           sine, period 30 s
;
; Alternative without the (http ...) block: mogbus --web profiles/minimal.scm

(server
  (mode tcp)
  (port 1502)
  (bind "127.0.0.1")

  ; The endpoint is off unless (http ...) is present. It must use another port than Modbus.
  (http
    (port 9102)           ; default 9102
    (bind "127.0.0.1")    ; default 127.0.0.1; keep it on loopback, there is no authentication
    (control on)          ; /api/: read and write registers, set and clear faults (logs a WARN)
    (ui on)))             ; the web page on /; it needs and turns on the control API

(device
  (name "Process rig")
  (unit-id 1)

  ; A setpoint you can write from the UI or from a Modbus client.
  (holding 0 (type s16) (static 450) (note "Setpoint (0.1 C)"))

  ; Follows the setpoint (lag model) with a little noise: change the setpoint and watch it move.
  (input 0 (type s16)
    (lag (target (holding 0)) (tau-ms 20000) (initial 200))
    (noise (amplitude 1.0) (every-ms 500) (distribution gaussian)))

  ; A ramp: rises 1 % every 500 ms and bounces back at the ends.
  (input 1 (ramp (from 0) (to 100) (step 1) (every-ms 500) (mode bounce)))

  ; A sine model: a smooth wave to look at in the Plot view.
  (input 2 (type s16)
    (sine (offset 0) (amplitude 1000) (period-ms 30000))))
