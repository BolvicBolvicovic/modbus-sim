# Step 4: Guard clients and keep records

> **Summary:** Install an access hook that vets client writes, react after accepted writes, log events in file records, serve an alarm FIFO, answer a vendor function code and watch or rewrite frames.

## The access hook

The real controller does not accept every write. An *access hook* gives the simulated one the same
manners: \ref mogbus::core::Device::set_access_hook "Device.set_access_hook()" installs a callable
that runs on every Modbus client request to the device (never on host calls) and either lets it
through or answers an exception. The example's hook is an instance of its class `Policy`, which keeps
the device, the `Process` of [step 3](python-simulate.md), a list `audit` of the writes it accepted and
a counter `refused`. It refuses setpoints outside 0.5 to 4.5 m, half a float32 and modes above 2:

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
            if first <= MODE <= last and values[MODE - first] > MANUAL:
                return self.refuse(access)
        self.audit.append((access.function_code, access.space.name.lower(), access.address, list(values)))
        return None

    def refuse(self, access):
        self.refused += 1
        return 3                    # illegal data value
```

and it is installed on the device with:

```python
policy = Policy(dev, process)
dev.set_access_hook(policy)
```

The hook receives an \ref mogbus::core::Access "Access" with the kind (read or write), the target, the
table, the first address, the count, the raw values about to be written and the function code.
Returning `None` lets the request go on; returning a code from 1 to 255 answers that Modbus exception
and the write changes nothing. The same hook is called for file records, FIFO queues and CANopen
objects, which is why it checks `access.target` first. Reads reach it too (a hook may refresh a value
just before it is read). Here, `values[i]` and `values[i + 1]` are the two registers of the float32
the client is writing, which the hook decodes with `struct`.

Now a client (`plc`, the example's `Client`) that misbehaves: it writes an out-of-range setpoint, then
a valid one, then switches to manual mode and writes the run command:

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
> it must stay short. See [step 5](python-lifetimes.md#threads-and-locks).

## Reacting after a write

The access hook runs before the write, so it is the wrong place to record that something changed: a
later hook, or the write itself, may still refuse it. \ref mogbus::core::Device::set_written_hook "Device.set_written_hook()"
installs a callable that runs after a client write was applied (the equivalent of pymodbus
`setValues`), once per written range, with the same `Access` and `values` holding what was stored. It
does not run for refused writes or for host calls such as `set_value()`, so it may call them without
looping. The example uses the method `written` of the same `Policy` to log the setpoint change in the
event log below (`self.process.event(...)` appends an event, see the next section):

```python
def written(self, access):
    if (access.target == mogbus.AccessTarget.TABLE and
            access.space == mogbus.Space.HOLDING and
            access.address <= SETPOINT < access.address + access.count and
            self.process is not None):
        self.process.event(EV_SETPOINT)

dev.set_written_hook(policy.written)
```

An exception in the hook is printed and swallowed: the write stays applied. Faults do not change
this: the hook also runs when the response is then dropped.

## An event log and an alarm FIFO

The file record and the FIFO added in [step 1](python-device.md#file-records-and-a-fifo) are filled
from the host with \ref mogbus::core::Device::write_file_records "Device.write_file_records()" and
\ref mogbus::core::Device::set_fifo "Device.set_fifo()" (and their `read_` partners). The event log
(file 1) keeps the ten most recent event codes, shifted at every event; the FIFO keeps the most recent
alarms. `Process.event(code)` appends to both; the codes are the `EV_*` constants of the example, such
as `EV_PUMP_ON`, `EV_HIGH_ON` or `EV_SETPOINT`:

```python
def event(self, code):
    log = self.dev.read_file_records(EVENT_FILE, 0, EVENT_RECORDS)
    self.dev.write_file_records(EVENT_FILE, 0, log[1:] + [code])
    if code in (EV_HIGH_ON, EV_LOW_ON):
        self.alarms = (self.alarms + [code])[-31:]
        self.dev.set_fifo(ALARM_FIFO, self.alarms)
```

`tick()` of step 3 calls `event()` when the pump or an alarm changes state. Now the operator switches
to manual mode and leaves the pump off: the tank fills until the high-level alarm, then the pump is
started and the tank empties until the low-level alarm. The client reads both stores with its
`file_records()` and `fifo()` methods:

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

The codes: 5 and 6 pump on and off, 7 a setpoint change by a client, 1 and 2 high alarm on and off, 3
low alarm on. The FIFO keeps only alarm-on events, oldest first, and a read does not consume it.

> **Note:** Clients can write file records (function code 0x15) unless your access hook vetoes them.
> The example's hook does not refuse them; add a rule on `access.target ==
> mogbus.AccessTarget.FILE_RECORD` for a log that only the host may write.

## A vendor function code

A device can also answer function codes the standard does not define, such as the vendor codes of commercial PLCs. \ref mogbus::core::Device::add_function_code "Device.add_function_code(code, callback)" defines one with a Python callback that gets the request data and returns the reply. The station answers 0x41 (`FC_STATUS_BLOCK`), "read station status block", with a block computed from the live values when the request arrives. In the example, `add_status_block(dev, process, policy)` defines it; `status` is the callback, and the reply carries the block id, the mode, the alarm and pump flags, the level in cm, the outflow in 0.1 m3/h and the hook's refusal count:

```python
def status(request):
    if request != b"\x01":
        return 3                                  # an int is a Modbus exception code
    flags = process.high | (process.low << 1) | (process.pump << 2)
    return struct.pack(">BBBHHH", 1, int(dev.get_value("holding", MODE)), flags,
                       round(dev.get_value("input", LEVEL) * 100),
                       round(dev.get_value("input", OUTFLOW) * 10), policy.refused)

dev.add_function_code(FC_STATUS_BLOCK, status)
```

The callback returns bytes (the reply data, at most 252), an int (an exception code) or None (no response); an exception raised inside it is printed and answered with exception 04. Like the access hook it runs on the server thread with the lock held, so it may read the device. The client side is one call, `call(pdu)`, which sends a raw request (the function code and its data) and returns the reply:

```python
block = plc.call(struct.pack(">BB", FC_STATUS_BLOCK, 1))
```

The request goes through the same path as any other: faults, listen-only, counters and metrics apply. See [user-defined function codes](../modbus/user-defined-function-codes.md#programmable-from-c-and-python) for the rules.

## Watching and rewriting frames

\ref mogbus::core::Server::set_frame_hook "Server.set_frame_hook()" shows every frame a server
receives and sends (the counterpart of pymodbus `trace_packet`) and lets a test rewrite or drop
them, which fixed faults cannot do. The hook gets a `Frame` (`direction`, `mode`, `peer`, `data`,
`capacity`) and returns `None` to pass it on, the replacement bytes, or `b""` to drop it. This example
is not part of the tutorial program: `server` is the started server of step 2 and `log` a list. It
records every frame and breaks the last byte of every answer:

```python
def hook(frame):
    log.append((frame.direction.name, frame.data.hex()))
    if frame.direction == mogbus.FrameDirection.SENT:
        return frame.data[:-1] + bytes([frame.data[-1] ^ 0xFF])   # the client sees a wrong last byte

server.set_frame_hook(hook)
```

Received frames are shown before they are validated, so the rewritten bytes are handled as if they
had arrived; sent frames are shown after fault injection and just before they are written. A
replacement longer than `capacity` drops the frame and logs a warning, and an exception in the hook
is printed and lets the frame through. The same rules per transport are in
[Guard clients and keep records](../library/library-hooks.md#watching-and-rewriting-frames).

## Next

- [Step 5: Observe, shut down, lifetimes and threading](python-lifetimes.md)

## See also

- [Step 4 in C](../library/library-hooks.md)
- [Writes are all-or-nothing](../modbus/writes.md)
- [File record reads (0x14)](../modbus/fc14-read-file-record.md) and [FIFO reads (0x18)](../modbus/fc18-read-fifo-queue.md)
- \ref core.py "Python API reference"
