# Driving the process from the host

Make the simulated station behave like a plant: the tank level follows the pump, alarms follow the level, and the rules of the real controller refuse bad writes.

A Modbus client sees a device only through its tables. The *process* behind them (water in, water
out, a motor drawing current) is yours to model, and the host side of the API is how you drive it.
This page continues [the tutorial](python-start.md) with steps 3 to 5.

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

## Step 3: the tank

`Process` in the example is plain Python. Its `tick(dt)` advances `dt` simulated seconds: it reads
what the clients wrote, decides whether the pump runs, integrates the level and writes the
measurements back:

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

Because the model is a function of simulated time, a test can fast-forward it (`tick(10.0)` is ten
seconds) instead of sleeping. The example moves a minute at a time while the client polls:

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
sees the same level, the pump feedback and the run seconds (a `u32` in two registers; the example prints the low word, which is the second) as the real controller would report them.

## Step 4: rules, and an audit trail

The real controller does not accept every write. An *access hook* gives the simulated one the same
manners: \ref mogbus::core::Device::set_access_hook "Device.set_access_hook()" installs a callable
that runs on every Modbus client request to the device (never on host calls) and either lets it
through or answers an exception. The example hook refuses setpoints outside 0.5 to 4.5 m, half a
float32 and modes above 2, and keeps a list of the writes it accepted:

```python
class Policy:
    def __call__(self, access):
        if access.kind != mogbus.AccessKind.WRITE or access.target != mogbus.AccessTarget.TABLE:
            return None
        values = access.values
        first, last = access.address, access.address + access.count - 1
        if access.space == mogbus.Space.HOLDING:
            if first <= SETPOINT + 1 and last >= SETPOINT:
                # The setpoint is a float32 in two registers: both must be written together.
                if first > SETPOINT or last < SETPOINT + 1:
                    return self.refuse(access)
                i = SETPOINT - first
                setpoint = struct.unpack(">f", struct.pack(">HH", values[i], values[i + 1]))[0]
                if not SETPOINT_RANGE[0] <= setpoint <= SETPOINT_RANGE[1]:
                    return self.refuse(access)
                ...
            if first <= MODE <= last and values[MODE - first] > MANUAL:
                return self.refuse(access)
        self.audit.append((access.function_code, access.space.name.lower(), access.address, list(values)))
        return None

    def refuse(self, access):
        self.refused += 1
        return 3                    # illegal data value
```

The hook receives an \ref mogbus::core::Access "Access" with the kind, table, address, count, the raw
values about to be written and the function code. Returning `None` lets the request go on; returning
a code from 1 to 255 answers that Modbus exception and the write changes nothing. The same hook is
called for file records, FIFO queues and CANopen objects, which is why it checks `access.target`
first. Reads reach it too (a hook may refresh a value just before it is read).

Now a client that misbehaves:

```python
try:
    plc.write_f32(SETPOINT, 9.0)
except ModbusException as exc:
    print("setpoint 9.0 m:", exc)
plc.write_f32(SETPOINT, 3.0)
print("setpoint now:", plc.read_f32(3, SETPOINT))
plc.write_register(MODE, MANUAL)
plc.write_coil(RUN_COMMAND, False)
print("audit:", policy.audit)
```

```text
setpoint 9.0 m: function 0x10 failed with exception 03
setpoint now: 3.0
audit: [(16, 'holding', 0, [16448, 0]), (6, 'holding', 2, [2]), (5, 'coil', 0, [0])]
```

`16448, 0` is 3.0 as a float32 (`0x4040 0x0000`). The refused writes are not in the audit trail: the
hook only records what it let through. Writing to the site name never reaches the hook: the point is
`read_only`, so the request fails with exception 02 first.

> **Note:** The hook runs on the server thread with the context lock held. The lock is reentrant, so
> the hook can call the methods of its own device (the example's hook appends to the event log), but
> it must stay short. See [Threads, locks and shutdown](python-threading.md).

## Step 5: alarms, the event log and the alarm FIFO

Besides the four tables a device can hold *file records* (read with function code 0x14, written
with 0x15) and *FIFO queues* (read with 0x18). They are built and changed from the host:
\ref mogbus::core::Device::add_file_record "Device.add_file_record()",
\ref mogbus::core::Device::write_file_records "Device.write_file_records()" and
\ref mogbus::core::Device::set_fifo "Device.set_fifo()" (and their `read_` and `add_` partners).
The station keeps an event log in file 1 (ten records, one register each, shifted at every event) and
the most recent alarms in FIFO 100:

```python
def event(self, code):
    log = self.dev.read_file_records(EVENT_FILE, 0, EVENT_RECORDS)
    self.dev.write_file_records(EVENT_FILE, 0, log[1:] + [code])
    if code in (EV_HIGH_ON, EV_LOW_ON):
        self.alarms = (self.alarms + [code])[-31:]
        self.dev.set_fifo(ALARM_FIFO, self.alarms)
```

Now the operator switches to manual mode and leaves the pump off: the tank fills until the
high-level alarm, then the pump is started and the tank empties until the low-level alarm:

```python
while not process.high:
    process.tick(10.0)
print("high-level alarm:", plc.discrete(HIGH_ALARM)[0], "level", round(plc.read_f32(4, LEVEL), 2))
plc.write_coil(RUN_COMMAND, True)
while not process.low:
    process.tick(10.0)
print("low-level alarm:", plc.discrete(LOW_ALARM)[0], "level", round(plc.read_f32(4, LEVEL), 2))
print("event log (file 1):", plc.file_records(EVENT_FILE, 0, EVENT_RECORDS))
print("alarm FIFO:", plc.fifo(ALARM_FIFO))
```

```text
high-level alarm: 1 level 4.0
low-level alarm: 1 level 0.46
event log (file 1): [0, 5, 6, 5, 6, 7, 1, 5, 2, 3]
alarm FIFO: [1, 3]
```

The codes are the `EV_*` constants of the example: 5 and 6 pump on and off, 7 a setpoint change by a
client, 1 and 2 high alarm on and off, 3 low alarm on. The FIFO keeps only alarm-on events, oldest
first, and a read does not consume it.

> **Note:** File records and FIFOs are config-backed storage in the protocol (a `(file ...)` or
> `(fifo ...)` form in a config file) and host-built storage here; clients can write file records
> (function code 0x15) unless your hook vetoes them.

Next: [Threads, locks and shutdown](python-threading.md) runs this model beside the server, and
[Logs, metrics, configs and runtime control](python-observe.md) shows what the station reports about
itself.

## See also

- [Python tutorial](python-start.md)
- \ref core.py "Python API reference"
- [Writes are all-or-nothing](../modbus/writes.md)
- [File record reads (0x14)](../modbus/fc14-read-file-record.md) and [FIFO reads (0x18)](../modbus/fc18-read-fifo-queue.md)
