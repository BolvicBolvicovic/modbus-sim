# Profile catalog

The ready-made profiles in the repository, what each device is and what it shows. Each profile starts with a comment listing its register map, and every one passes `mogbus --check`. All serve unit 1 on `127.0.0.1:1502` unless the section says otherwise. See [Using a profile](using-profiles.md) to run one and [Write your own profile](write-your-own-profile.md) to adapt it.

## [`minimal.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/minimal.scm)

The smallest useful device, unit 1 over TCP: holding 0 is fixed at 42 and holding 1 counts 0, 10, 20 ... 100 and starts again, one step per second. Start here to check that a client connects.

## [`modbus-udp.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/modbus-udp.scm)

The same two registers served over Modbus UDP: one MBAP frame per datagram, and the reply goes back to the sender. A small delay fault (10% of answers 50 ms late) shows that faults work on UDP. The transport is one line of the `server` form:

```scheme
(server
  (mode udp)
  (port 1502))
```

## [`ascii-udp.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/ascii-udp.scm)

The same two registers as ASCII frames over UDP: each datagram carries exactly one `:`...CR LF frame, and a malformed datagram (missing CR LF, extra bytes, two frames) gets no reply. It has the same 10% delay fault; `disconnect` is not available because UDP has no connection.

```scheme
(server
  (mode ascii-udp)
  (port 1502))
```

## [`rtu-udp.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/rtu-udp.scm)

The same two registers as RTU frames over UDP: one frame (`[addr][PDU][CRC lo hi]`) per datagram, no frame-gap timing, and a bad CRC, an unknown unit or unit 0 get no reply. Same delay fault.

```scheme
(server
  (mode rtu-udp)
  (port 1502))
```

## [`power-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/power-meter.scm)

A generic three-phase electricity meter: the measured values are float32 input registers (two registers each, high word first) moved by bouncing ramps: three voltages, three currents, active power, frequency, power factor and an energy counter in kWh that counts up. Two holding registers are settings (CT ratio, demand period), coil 0 resets the energy counter and discrete 0 is the phase-loss alarm. Every value is a ramp with its own period so the readings never move in step.

## [`energy-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/energy-meter.scm)

A revenue-style energy meter that shows the register layouts real meters use beyond a plain float32: a 64-bit energy counter (u64), a float64 power, byte-swapped float32 voltage and current (BADC) and two ASCII string registers, a serial number (NUL padded) and a model name (space padded). Holding 1 is a read-only firmware version, so a client write to it gets exception 02. The special points:

```scheme
  (input 8  (type float32) (byte-order little)
    (ramp (from 228) (to 232) (step 0.5) (every-ms 2000) (mode bounce)))
  (input 10 (type float32) (byte-order little)
    (ramp (from 4.0) (to 6.0) (step 0.1) (every-ms 1500) (mode bounce)))
  (input 20 (type string) (length 8) (static "EM3-2026-000417"))
  (input 28 (type string) (length 8) (padding space) (static "EM-100 3P"))
```

`(byte-order little)` swaps the two bytes of every register; decode BADC by swapping them back and reading a normal big-endian float32 ([byte order](../config/points/byte-order.md), [string](../config/points/string.md)).

## [`temperature-controller.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/temperature-controller.scm)

A single-loop temperature controller with signed values in tenths of a degree: a process value that drifts between 18.0 and 24.0 C (input 0, s16), an output power in percent, a writable setpoint (holding 0, s16) and mode (off, auto, manual), a run-enable coil and two alarm bits (high temperature, sensor fault). It shows signed 16-bit registers.

## [`heater.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/heater.scm)

An electric heater with a thermostat: the water temperature (input 0) follows the writable setpoint (holding 0, 45.0 C) with a 60 second time constant and gaussian noise, starting at 20.0 C; the ambient temperature (input 1) is a slow daily sine around 20 C. Write 600 to the setpoint and watch input 0 climb towards 60.0 C. The `lag` behaviour follows another point:

```scheme
  (holding 0 (type s16) (static 450) (note "Setpoint (0.1 C)"))

  (input 0 (type s16)
    (lag (target (holding 0)) (tau-ms 60000) (initial 200))
    (noise (amplitude 1.5) (every-ms 1000) (distribution gaussian)))
```

See [`lag`](../config/behaviours/lag.md) and [`noise`](../config/behaviours/noise.md).

## [`aging-sensor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/aging-sensor.scm)

A pressure transmitter that wears out, in hundredths of a bar. Input 0 reads the process value plus a little noise for an hour, then drifts upwards and gets noisier, sticks at its current value after 3 hours and fails after 6 hours (reads that cover it answer exception 04); input 1 is the same process value from a healthy sensor, to compare with. A restart repairs the sensor. The timeline is one `degradation` form:

```scheme
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
```

See [`degradation`](../config/behaviours/degradation.md).

## [`vfd-drive.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/vfd-drive.scm)

A variable frequency drive that "accelerates" after start-up: the output frequency (0.01 Hz) and the motor current climb and then hold (ramps with `(mode hold)`), next to a DC bus voltage, a drive temperature and a 32-bit energy counter (u32, high word first). Holding registers carry a control word and a frequency setpoint, coils a run command and a fault reset, discrete inputs the running and fault flags. The fault code is read-only:

```scheme
  (holding 0 (static 0))
  (holding 1 (static 5000))
  (holding 2 (access read-only) (static 0))
```

## [`motor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/motor.scm)

A motor driven by a scripted state machine: a client sets coil 0 to run it and the machine goes idle, starting, running (the speed climbs to the limit in holding 0 and is reported in input 0, with discrete 0 as the running flag), and trips at a random moment between one and ten minutes after it runs; coil 1 resets the trip. The points the machine writes belong to it, so a client write to them is refused with exception 02.

```scheme
  (machine motor
    (initial idle)
    (var speed 0)

    (state idle
      (on-enter (set! (discrete 0) #f) (set! speed 0) (set! (input 0) 0)))

    (state starting
      (on-tick (set! speed (min (holding 0) (+ speed (/ (* 40 (elapsed-ms)) 1000))))
               (set! (input 0) speed)))

    (state running
      (on-enter (set! (discrete 0) #t))
      (on-tick  (set! (input 0) (holding 0))))

    (state tripped
      (on-enter (set! (discrete 0) #f) (set! speed 0) (set! (input 0) 0)))

    (transition idle     starting (when (coil 0)))
    (transition starting running  (after-ms 3000))
    (transition running  idle     (when (not (coil 0))))
    (transition running  tripped  (after-ms (random 60000 600000)))
    (transition tripped  idle     (when (coil 1)))))
```

See [machines](../config/machines/index.md).

## [`flow-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/flow-meter.scm)

A liquid flow meter with a totalizer: the flow rate (m3/h) and the fluid temperature as float32 ramps, a 32-bit totalizer that counts up (u32 over two registers), a reset coil and an empty-pipe alarm. It is the profile the Python tutorial loads.

## [`io-module.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/io-module.scm)

A remote I/O block: 16 digital inputs (8 off, 8 on), 16 writable digital outputs, 8 analog inputs in microamps of a 4-20 mA loop (channel 0 sweeps the full range, the rest are fixed) and 4 writable analog outputs. It uses `(count N)` to define many points in one entry:

```scheme
  (discrete 0 (count 8) (static off))
  (discrete 8 (count 8) (static on))

  (coil 0 (count 16) (static off))

  (input 0 (ramp (from 4000) (to 20000) (step 160) (every-ms 1000) (mode bounce)))
  (input 1 (count 7) (static 12000))

  (holding 0 (count 4) (static 4000)))
```

See [`count`](../config/points/count.md).

## [`slow-sensor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/slow-sensor.scm)

A temperature sensor that sometimes answers late: about 30% of the answers arrive 1.5 s late, the same ones on every run because of the server seed. Use it to test client timeouts and retries; other clients are not blocked while an answer waits. Input 0 is the temperature in tenths of a degree.

```scheme
(server
  (mode tcp)
  (port 1502)
  (seed 7))     ; same seed = same sequence of late answers every run

(device
  (name "Slow temperature sensor")
  (unit-id 1)
  (fault (kind delay) (delay-ms 1500) (percent 30))

  (input 0 (type s16) (ramp (from 200) (to 250) (step 1) (every-ms 2000) (mode bounce))))
```

See [`delay`](../config/faults/delay.md) and [`seed`](../config/server/seed.md).

## [`unreliable-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/unreliable-meter.scm)

An energy counter (float32, kWh, counting up) that goes silent: about 20% of the answers never arrive, so the client sees a timeout. Use it to test retry logic and "device offline" handling.

```scheme
(server
  (mode tcp)
  (port 1502)
  (seed 11))

(device
  (name "Unreliable energy meter")
  (unit-id 1)
  (fault (kind drop) (percent 20))

  (input 0 (type float32) (ramp (from 0) (to 100000) (step 0.25) (every-ms 1000) (mode wrap))))
```

See [`drop`](../config/faults/drop.md).

## [`two-devices.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/two-devices.scm)

Two devices behind one port, told apart by the Modbus unit id, like a gateway: unit 1 is a room temperature sensor (s16 ramp), unit 2 a water flow meter (float32 ramp). An unknown unit id gets exception 0B (gateway target failed to respond). See [unit ids](../modbus/unit-ids.md).

## [`multi-server.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/multi-server.scm)

Several servers in one file, running together: Modbus TCP on 1502 (`plant`), RTU-over-TCP on 1503 (`gateway`) and Modbus UDP on 1502 (`legacy`; TCP and UDP use separate port spaces). Unit 1 (a meter whose holding 0 is a setpoint) is served by the first two, so it can be reached over two transports with the same registers; unit 2 (a sensor) only by the UDP one. Servers are named with `(id ...)` and a device lists them with `(servers ...)`:

```scheme
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
```

See [`id`](../config/server/id.md) and [`servers`](../config/device/servers.md).

## [`http-endpoint.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/http-endpoint.scm)

A process rig with the HTTP endpoint on, to try the health checks, metrics, control API and web UI on `127.0.0.1:9102`: a writable setpoint (holding 0), a temperature that lags it with noise (input 0), a bouncing tank-level ramp (input 1) and a 30 second sine wave (input 2). Open the web UI, change the setpoint and plot the inputs. The endpoint is the `(http ...)` block of the server:

```scheme
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
```

See [`http`](../config/server/http.md) and [Web UI](../observability/web-ui.md).

## [`vendor-function-codes.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/vendor-function-codes.scm)

A vendor-style device with user-defined function codes: a status word, a serial number, a setpoint and two live inputs, plus function code 0x41 ("Vendor info": a fixed reply, live registers or an echo, depending on the request data) and 0x64 ("Vendor service": an acknowledge, an exception or no response at all). A client needs to send raw PDUs to try them; the comment of the file lists what each request returns.

```scheme
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
```

See [`function-code`](../config/device/function-code.md) and [user-defined function codes](../modbus/user-defined-function-codes.md).

## [`read-actions.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/read-actions.scm)

Point actions: registers that change when a client reads or writes them, unit 1 over TCP. Holding 0 is a read counter that wraps (it counts by 5 and wraps at 100), holding 1 is twice that counter computed on every read, holding 2 is the time since it was last read, and writing 1 to coil 0 resets the counter. A reusable action with parameters, and its use on a point:

```scheme
(define-action rolling-counter
  (param step 1)
  (param limit 65535)
  (set! self (if (>= (+ self step) limit) 0 (+ self step))))

(device (unit-id 1)
  (holding 0 (static 0)
    (on-read (rolling-counter (step 5) (limit 100)))))
```

See [point actions](../config/actions/index.md), [`on-read`](../config/actions/on-read.md) and [`on-write`](../config/actions/on-write.md).

## See also

- [Using a profile](using-profiles.md)
- [Write your own profile](write-your-own-profile.md)
- [Config reference](../config/format.md)
