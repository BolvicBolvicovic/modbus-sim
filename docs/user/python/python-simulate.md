# Step 3: Simulate the process from the host

> **Summary:** Make the simulated station behave like a plant: the tank level follows the pump and the alarms follow the level, all driven from the program, and see how host writes differ from client writes.

A Modbus client sees a device only through its tables. The *process* behind them (water in, water
out, a motor drawing current) is yours to model, and the host side of the API is how you drive it.
Nothing is computed per request: values change when the host changes them (or, for ramps, when they
are read).

## Two kinds of write

The station has two sources of change, and mogbus keeps them apart:

- **Clients** change what a real client may change: setpoint, mode and run command. What they write is
  what \ref mogbus::core::Device::get_value "Device.get_value()" returns next, so the host reads
  its inputs the same way.
- **The host** (your process model) changes what the plant would measure, with
  \ref mogbus::core::Device::set_value "Device.set_value()". The point becomes *host-owned*: a ramp
  stops moving it, a client write to it is refused with exception 02, and `read_only` is ignored. This is what you
  want for the level, the flows and the alarms.

\ref mogbus::core::Device::write "Device.write()" and \ref mogbus::core::Device::read "Device.read()"
are the raw-register versions (they behave as a client request, without the network), useful to
check the encoding.

## Pinning the ramp

The inlet flow of step 1 is a ramp, which would make the tank's inflow change every second. The
simulation needs a repeatable inflow, so the example takes the point over from the clock with
`set_value` (`dev` is the device built in [step 1](python-device.md)); from now on it is host-owned and
stays at 40 m3/h:

```python
dev.set_value("input", INFLOW, 40.0)
```

## The tank

`Process` in the example is a plain Python class that holds the device (`self.dev`, the one from
step 1) and the state of the plant (`self.pump`, `self.seconds`, `self.high`, `self.low`). Its
`tick(dt)` advances `dt` simulated seconds: it reads what the clients wrote, decides whether the pump
runs, integrates the level and writes the measurements back. The constants `PUMP_FLOW`, `TANK_AREA` and
`TANK_HEIGHT` describe the plant; the elided lines raise and clear the alarms, which
[step 4](python-hooks.md) logs:

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

The first lines read what clients can change (the setpoint, the mode and, in manual mode, the run
command) with `get_value`; the last five publish the plant's measurements with `set_value`. In auto
mode the pump starts 0.25 m above the setpoint and stops 0.25 m below it.

Because the model is a function of simulated time, a test can fast-forward it (`tick(10.0)` is ten
seconds) instead of sleeping. With `process = Process(dev)` and `plc` the example's client, the
program moves a minute at a time (six ticks of ten seconds) while the client polls the level, the pump
feedback and the run seconds:

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
band) the auto mode starts the pump, which takes the level down faster than it fills. Your client
sees the same level, the pump feedback and the run seconds (a `u32` in two registers; the example
prints the low word, which is the second) as the real controller would report them.

## Next

- [Step 4: Guard clients and keep records](python-hooks.md)

## See also

- [Step 3 in C](../library/library-simulate.md)
- \ref core.py "Python API reference"
