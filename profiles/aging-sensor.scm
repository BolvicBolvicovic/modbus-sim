; aging-sensor.scm - pressure transmitter that wears out.
;
; Pressures are whole numbers in hundredths of a bar (500 = 5.00 bar).
;
; Input registers (read-only):
;   0  Pressure (0.01 bar)    the aging sensor, see the timeline below
;   1  Reference pressure     the same process value from a healthy sensor, to compare with
;
; Timeline, in time since the simulator (or device) started; a restart repairs the sensor:
;   0 - 1 h      reads the process value plus a little noise
;   1 h on       drifts +6 (0.06 bar) per hour and gets noisier by +4 (0.04 bar) per hour
;   3 h          sticks: from then on it always returns the value it had at that moment
;   6 h          fails: reads that cover input 0 answer exception 04 (slave device failure)
;
; Read input 1 alone after 6 h to see that the process is still fine.

(server (mode tcp) (port 1502))

(device
  (name "Aging pressure transmitter")
  (unit-id 1)

  (input 0 (type u16)
    (sine (offset 500) (amplitude 40) (period-ms 600000))
    (noise (amplitude 1) (every-ms 1000))
    (degradation
      (after-ms 3600000)
      (drift-per-hour 6)
      (noise-growth-per-hour 4)
      (stuck-after-ms 10800000)
      (fail-after-ms 21600000))
    (note "Pressure (0.01 bar), degrades over time"))

  (input 1 (type u16)
    (sine (offset 500) (amplitude 40) (period-ms 600000))
    (note "Reference pressure (0.01 bar), healthy")))
