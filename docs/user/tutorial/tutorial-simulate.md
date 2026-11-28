# Step 3: Make it behave

> **Summary:** Make the station move like a plant: the tank fills, the pump runs, the alarms follow the level and the motor current follows the pump, either described in the config (behaviours and a machine) or driven from the host program.

A client sees a device only through its tables. The *process* behind them (water in, water out, a motor
drawing current) is yours to model, and the routes do it differently:

- **Config**: you describe the process in the file, and the simulator computes it. Behaviours and machines are evaluated when a request arrives, not on a timer, so an idle server costs nothing. This is the only route that has [`sine`](../config/behaviours/sine.md), [`lag`](../config/behaviours/lag.md), [`noise`](../config/behaviours/noise.md) and [machines](../config/machines/index.md).
- **C and Python**: the host program owns the model. Your own code decides the tank level and pushes the measurements to the device with the host side of the API, between steps of the server loop. Nothing is computed per request.

## Live values without code

Replace the single input of step 1 with live ones (config route). Outflow is a noisy sine, the motor
current follows the "pump running" bit with a lag, and the clock register shows the time of the machine
running mogbus:

```scheme
(server (mode tcp) (port 1502) (seed 7))

(device
  (name "Pump station")
  (unit-id 1)

  (holding 0 (type float32) (static 2.5))
  (holding 2 (type u16) (static 1))
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))
  (coil 0)
  (discrete 0)
  (discrete 1)
  (discrete 2)

  (input 0 (type float32) (static 2.5))
  (input 2 (type float32)
    (sine (offset 40) (amplitude 6) (period-ms 120000))
    (noise (amplitude 0.8) (every-ms 500) (distribution gaussian)))
  (input 4 (type u16)
    (lag (target (discrete 2)) (tau-ms 1500) (gain 120))
    (noise (amplitude 2) (every-ms 250)))
  (input 10 (type u32) (static 0))
  (input 20 (type u32) (clock unix)))
```

- `sine` is `offset + amplitude * sin(...)`: 40 m3/h plus or minus 6 over a two-minute period; `noise` adds a reproducible disturbance on top.
- `lag` follows a goal like a first-order system: the current heads for `120` (12.0 A) while the "pump running" bit is 1 and back to 0 when it is 0, closing 63% of the gap every 1.5 s. A target can be any configured point.
- [`clock`](../config/behaviours/clock.md) shows `unix` seconds; it needs an integer type of at least 32 bits.
- `(seed 7)` makes the random parts of machines reproducible ([seed](../config/server/seed.md)); the noise is reproducible on its own.

## The process

Level and run seconds should depend on what the pump does. The model is the same in every route: the
tank fills while the pump is off and drains while it runs; in auto mode (mode 1) the pump starts above
the setpoint plus a band and stops below the setpoint minus the band, in manual mode (2) it follows the
run command coil, in mode 0 it is off; the high alarm trips near the top of the tank and the low alarm
near the bottom.

In the config it is a [machine](../config/machines/index.md): a small state machine that owns some
points, added inside the device next to the points. The tank gains 2 cm per second with the pump off and
loses 4 cm per second with it on, and the band is 0.3 m. In C and Python it is a function that runs
between server steps: it reads what clients wrote (setpoint, mode, run command), decides whether the
pump runs, integrates the level over `dt` simulated seconds and publishes the measurements. The C
program's `simulate(st, dt)` advances 0.02 m/s up and 0.05 m/s of drain (band 0.2 m); the Python class
`Process` has `tick(dt)` with m3/h flows over a 2 m2 tank (band 0.25 m). The addresses and alarm limits differ per
program but the idea does not.

```scheme
  (machine pump
    (initial stopped)
    (var seconds 0)

    (state stopped
      (on-enter (set! (discrete 2) #f))
      (on-tick (set! (input 0) (min 5 (+ (input 0) (/ (* 0.02 (elapsed-ms)) 1000))))))
    (state running
      (on-enter (set! (discrete 2) #t))
      (on-tick (set! (input 0) (max 0 (- (input 0) (/ (* 0.04 (elapsed-ms)) 1000))))
               (set! seconds (+ seconds (/ (elapsed-ms) 1000)))
               (set! (input 10) seconds)))

    (transition stopped running
      (when (or (and (= (holding 2) 1) (> (input 0) (+ (holding 0) 0.3)))
                (and (= (holding 2) 2) (coil 0)))))
    (transition running stopped
      (when (or (= (holding 2) 0)
                (and (= (holding 2) 1) (< (input 0) (- (holding 0) 0.3)))
                (and (= (holding 2) 2) (not (coil 0)))))))

  (machine alarms
    (initial watching)
    (state watching
      (on-tick (set! (discrete 0) (> (input 0) 4))
               (set! (discrete 1) (< (input 0) 1)))))
```

```c
/* the start of simulate(): what the clients wrote */
mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_MODE, &mode);
mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_SETPOINT, &setpoint);
mogbus_device_read(dev, MOGBUS_COIL, COIL_RUN, 1, &coil);
...
/* its end: publish the measurements */
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_LEVEL, st->level);
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_CURRENT, pump ? 85 : 0);
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_RUN_SECONDS, (double)(uint32_t)st->run_s);
mogbus_device_set_value(dev, MOGBUS_DISCRETE, DISC_HIGH, st->high);
```

```python
def tick(self, dt):
    dev = self.dev
    setpoint = dev.get_value("holding", SETPOINT)
    mode = int(dev.get_value("holding", MODE))
    level = dev.get_value("input", LEVEL)
    if mode == AUTO:
        if level >= setpoint + 0.25:
            self.pump = True
        elif level <= setpoint - 0.25:
            self.pump = False
    elif mode == MANUAL:
        self.pump = bool(dev.get_value("coil", RUN_COMMAND))
    else:
        self.pump = False
    outflow = PUMP_FLOW if self.pump else 0.0
    inflow = dev.get_value("input", INFLOW)
    level = min(max(level + (inflow - outflow) / 3600.0 * dt / TANK_AREA, 0.0), TANK_HEIGHT)
    ...
    dev.set_value("input", LEVEL, level)
    dev.set_value("input", OUTFLOW, outflow)
    dev.set_value("input", CURRENT, round(10 * (10 + 2 * level)) if self.pump else 0)
    dev.set_value("input", RUN_SECONDS, int(self.seconds))
    dev.set_value("discrete", RUNNING, self.pump)
```

- **Config.** `elapsed-ms` is the time since the machine last ran, so the result does not depend on how often a client polls. The `transition` lines are the control logic: `(holding 2)` is the mode, `(holding 0)` the setpoint and `(coil 0)` the run command, all of which a client can write. See [state](../config/machines/state.md), [transition](../config/machines/transition.md), [statements](../config/machines/statements.md) and [expressions](../config/machines/expressions.md). The second machine, `alarms`, shows that machines on one device can read each other's points.
- **C.** @ref mogbus_device_get_value "mogbus_device_get_value()" returns the decoded value of the point that starts at an address (a float32 comes back as the number, not two registers); @ref mogbus_device_read "mogbus_device_read()" returns raw registers and is exact for any type. Reads advance ramps first and never invoke the access hook. `simulate()` runs in the server loop of [step 2](tutorial-serve.md#who-steps-the-server).
- **Python.** `Process` holds the device and the state of the plant; the constants `PUMP_FLOW`, `TANK_AREA` and `TANK_HEIGHT` describe it, and the elided lines raise and clear the alarms, which [step 4](tutorial-guard.md) logs. Because the model is a function of simulated time, a test can fast-forward it (`tick(10.0)` is ten seconds) instead of sleeping.

## Reacting to reads and writes

A config can also run statements when a client reads or writes a point (config route only). The
motor-hours register of a station could count how often the SCADA polls it, and a coil could reset
the count; this snippet is the pattern of the
[`read-actions` profile](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/read-actions.scm), not part of the tutorial file:

```scheme
(define-action rolling-counter
  (param step 1)
  (param limit 65535)
  (set! self (if (>= (+ self step) limit) 0 (+ self step))))

(device (unit-id 1)
  (holding 0 (static 0)
    (on-read (rolling-counter (step 5) (limit 100)))))
```

See [point actions](../config/actions/index.md). A C or Python host does this with the access and
post-write hooks of [step 4](tutorial-guard.md) instead, which also see the request.

## Who owns a point

A point a config machine writes belongs to it: a client write to the level or the alarms fails with
exception 02, so the simulation keeps control ([point ownership](../config/machines/points.md)). In C
and Python the host does the same with `set_value`, and the two kinds of write stay apart:

- **Clients** change what a real client may change: setpoint, mode and run command. What they write is what `get_value` returns next, so the host reads its inputs the same way.
- **The host** changes what the plant would measure, with @ref mogbus_device_set_value "mogbus_device_set_value()" / \ref mogbus::core::Device::set_value "Device.set_value()". The point becomes *host-owned*: a ramp stops moving it, a client write to it is refused with exception 02, and `read_only` is ignored. It works on every table.

@ref mogbus_device_write "mogbus_device_write()" / \ref mogbus::core::Device::write "Device.write()" are the raw-register versions: they behave as a client request without the network (they turn a ramp static and leave the point writable by clients, which is why the run command coil keeps working in manual mode). The C program mirrors the pump into the coil with it outside manual mode:

```c
if (mode != MODE_MANUAL && coil != pump)
{
	uint16_t v = pump;

	mogbus_device_write(dev, MOGBUS_COIL, COIL_RUN, 1, &v);
}
```

| | `set_value` | `write` |
|---|---|---|
| Addressing | a point's first address, decoded number | raw registers |
| Read-only point / input / discrete | works | works |
| Afterwards a client write | exception 02 (host-owned) | allowed as before |
| Ramp | stops | becomes static |
| Machines | like a machine output | runs on-write handlers |

The Python inlet flow of step 1 is a ramp, which would make the tank's inflow change every second; the
simulation needs a repeatable inflow, so the program takes the point over from the clock (from now on
it is host-owned and stays at 40 m3/h):

```python
dev.set_value("input", INFLOW, 40.0)
```

Strings go through @ref mogbus_device_get_string "mogbus_device_get_string()" / `Device.get_string()` and
`set_string()`; unlike `set_value` they do not make the point host-owned. Full rules:
[host side in C](../library/library-simulate.md), [in Python](../python/python-simulate.md).

## Run it and watch

In the config route the machines run as soon as the server starts:

```bash
./mogbus examples/tutorial/pump-station.scm
```

and from a second terminal:

```bash
./tools/modbus_probe.py discrete 0 3                 # high, low, running
./tools/modbus_probe.py input 2 2 --as float32       # outflow
./tools/modbus_probe.py input 20 2 --as u32          # wall clock
./tools/modbus_probe.py --watch 5 input 0 2 --as float32
```

```text
[0, 0, 0]
[40.29308319091797]
[1795777594]
[2.5212199687957764]
[2.6212198734283447]
[2.721219778060913]
[2.821239709854126]
[2.62123966217041]
[2.4211995601654053]
[2.2211995124816895]
[2.0211594104766846]
```

`--watch` goes before the command. The level rises 0.1 m a poll, the pump starts above 2.8 m and the
level falls until it stops below 2.2 m. The Python program has no wall time to wait for: it moves a
minute at a time (six ticks of ten seconds) while the client polls the level, the pump feedback and the
run seconds:

```python
print("minute  level  pump  run-s")
for minute in range(0, 8):
    if minute:
        for _ in range(6):
            process.tick(10.0)
    print(f"{minute:6d}  {plc.read_f32(4, LEVEL):5.2f}  {plc.discrete(RUNNING)[0]:4d}  "
          f"{plc.input(RUN_SECONDS, 2)[1]:5d}")
```

```text
minute  level  pump  run-s
     0   2.00     0      0
     1   2.33     0      0
     2   2.67     0      0
     3   2.50     1     40
     4   2.33     0     80
     5   2.67     0     80
     6   2.50     1    120
     7   2.33     0    160
```

The inflow of 40 m3/h fills the 2 m2 tank by 0.33 m a minute; at 2.75 m (setpoint plus the 0.25 m
band) the auto mode starts the pump, which takes the level down faster than it fills. The run seconds
are a `u32` in two registers; the example prints the low word, which is the second. The C self-test
calls `simulate(st, 1.0)` directly to move simulated time without waiting. After a client switches the
station off, it calls it for 100 simulated seconds and checks what a client then reads:

```text
  ok   FC 06 switches the station off
  ok   100 s later the high-level alarm is set
  ok   FC 04 reads a level above 4 m
```

## Write from a client

Switch to manual mode and start the pump by hand. These commands work against the config server and
against the C program, which serves the same mode, run command and running bit:

```bash
./tools/modbus_probe.py write-reg 2 2        # mode 2 = manual
./tools/modbus_probe.py write-coil 0 on      # run command
./tools/modbus_probe.py discrete 2 1         # running -> [1]
./tools/modbus_probe.py input 10 2 --as u32  # run seconds, counting
```

With the config route poll `input 4` once a second and the motor current climbs towards 12 A (`lag`
follows its target as requests arrive, so a client that polls rarely sees it jump):

```bash
./tools/modbus_probe.py --watch 1 input 4 1
```

```text
[0]
[60]
[90]
[105]
[114]
[117]
```

Writes a real device would refuse are refused here too: a write to the read-only site name or to an
address with no point answers `error: exception 0x02: illegal data address`, and so does a client write
to the level, which the machine owns. Writing a ramp, sine or clock point freezes it at the written
value, which is how a test changes a "sensor" ([client writes](../config/behaviours/client-writes.md)).

## Next

- [Step 4: Guard clients and keep records](tutorial-guard.md)
