# Step 4: Guard clients and keep records

> **Summary:** Make the station refuse what the real PLC would refuse, react to accepted writes, keep an event log and an alarm FIFO and answer a vendor function code, with hooks in C and Python and with config keys where the config can do it.

The real controller does not accept every write, and it keeps records a data logger reads. Most of this
step is the library side: the callbacks (*hooks*) that run on the Modbus client path of a device are
the embedded counterpart of subclassing a datastore in pymodbus. The config route has only a few
equivalents, and they are said where they apply: `(access read-only)` (step 1), `(function-code ...)`
below, and the fixed `file` and `fifo` data of step 1.

## Refuse writes: the access hook

An *access hook* runs on every Modbus client request to one device (never on host calls) and either
lets it through or answers an exception. It is the place to refresh a value right before a read, or to
veto a write; a vetoed write changes nothing, so [writes stay all-or-nothing](../modbus/writes.md). The
station's rules:

- a setpoint outside 0.5 to 4.5 m, or half of the float32, is refused with exception 03;
- a mode above 2 is refused with exception 03;
- in the C program, while the mode is off every write but the mode register is refused with exception 04, and the event log is the host's (client writes to it get exception 01).

The C hook is installed with @ref mogbus_device_set_access_hook "mogbus_device_set_access_hook()",
passing the address of the program's `struct station` as the `user` pointer that the library hands back
on every call. It receives a @ref mogbus_access_t "mogbus_access_t": `kind` (read or write), the
`target`, the `function_code` and the fields the target uses (`space`, first `address`, `count` and, for writes, the
raw `values` about to be stored). Return 0 to let the request go on, or a Modbus exception code. The
Python hook is any callable, installed with \ref mogbus::core::Device::set_access_hook "Device.set_access_hook()";
it gets an \ref mogbus::core::Access "Access" with the same fields and returns `None` to let the request
through or an int from 1 to 255 for that exception.

```c
mogbus_device_set_access_hook(st.dev, on_access, &st);
...
mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_MODE, &mode);
if (mode == MODE_OFF &&
    !(a->space == MOGBUS_HOLDING && a->address == HOLD_MODE && a->count == 1))
{
	st->refused++;
	return 4;
}
...
double sp = float_of(a->values + (HOLD_SETPOINT - a->address));
if (!(sp >= 0.5 && sp <= 4.5))
	return 3;
```

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

policy = Policy(dev, process)
dev.set_access_hook(policy)
```

This part is C and Python only: the config route has just [`(access read-only)`](../config/points/access.md),
which refuses every client write to a point (the site name of step 1), and a range check or a refusal
that depends on the mode needs a hook. The hook
inspects both registers of the float32, because a client may write a range that overlaps the setpoint
partly (an FC 16 over holding 1-2): it checks that both are in the range or none, and decodes the
float itself. The C hook reads the mode with the host-side `mogbus_device_get_value()`, which does not
trigger the hook again. Now a client that misbehaves. The Python program writes an out-of-range
setpoint, then a valid one, then switches to manual mode and writes the run command:

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
hook only records what it let through. The C self-test tries each rule:

```text
  ok   FC 16 setpoint 9.0 m is refused by the hook: exception 03
  client write: FC 10, holding address 0, 2 item(s)
  ok   FC 16 setpoint 3.0 m is accepted
  ok   FC 06 mode 7 is refused: exception 03
  ok   FC 05 while off is refused: exception 04
  ok   FC 15 on the event log is refused: exception 01
```

An undefined address, a read-only point or a machine-driven point answers exception 02 without calling
the hook, which is why the read-only site name needs no rule. The hook runs on the thread that steps the
server with the context lock held, so it may call the other methods of its own device but must stay
short. Where it runs for every function code, and the targets it also covers (file records, FIFO queues,
CANopen): [hooks in C](../library/library-hooks.md#where-the-hook-runs),
[hooks in Python](../python/python-hooks.md#the-access-hook).

## React after a write

The access hook runs before the write, so it cannot tell that the write happened. A *post-write hook*
(@ref mogbus_device_set_written_hook "mogbus_device_set_written_hook()",
\ref mogbus::core::Device::set_written_hook "Device.set_written_hook()") runs after a Modbus client write
was applied, once per written range, with the same access and `values` holding what was stored (the
pymodbus `setValues` counterpart). It does not run for refused writes or host calls, so it may call them
without looping. The station uses it to put setpoint and mode changes in the event log:

```c
if (a->address <= HOLD_MODE && end > HOLD_MODE)
	log_event(st, EVT_MODE | a->values[HOLD_MODE - a->address]);
```

```python
def written(self, access):
    if (access.target == mogbus.AccessTarget.TABLE and
            access.space == mogbus.Space.HOLDING and
            access.address <= SETPOINT < access.address + access.count and
            self.process is not None):
        self.process.event(EV_SETPOINT)

dev.set_written_hook(policy.written)
```

In the C program `end` is the first address after the written range and `log_event()` is the event log
function below. Faults do not change this: the hook also runs when the response is then dropped. The C
self-test checks that a refused setpoint leaves the event counter alone and an accepted one advances it
by one:

```text
  ok   the refused write did not reach the post-write hook
  ok   the post-write hook logged the setpoint event
```

## An event log and a FIFO

The stores added in [step 1](tutorial-device.md#file-records-and-a-fifo) are filled from the host:
clients read the event log with FC 14 and the FIFO with FC 18, and the host appends with
@ref mogbus_device_write_file_records "mogbus_device_write_file_records()" and
@ref mogbus_device_set_fifo "mogbus_device_set_fifo()" (Python `write_file_records()` and `set_fifo()`,
plus `read_` partners). These are host calls, so they do not trigger the hooks.

- **C.** Record 1 counts the events and records 2 to 100 are a ring of codes (high byte what, `EVT_*` constants; low byte the value). The FIFO keeps the last 31 level samples (in centimetres, newest last) and is refreshed in the access hook right before a client reads it, so the queue costs nothing between reads.
- **Python.** The event log (file 1) keeps the ten most recent event codes, shifted at every event; the FIFO keeps the most recent alarms. `Process.event(code)` appends to both, and `tick()` of step 3 calls it when the pump or an alarm changes state.

```c
uint16_t slot = (uint16_t)(2 + st->events % (LOG_RECORDS - 1));
uint16_t count = (uint16_t)++st->events;

mogbus_device_write_file_records(st->dev, LOG_FILE, slot, 1, &code);
mogbus_device_write_file_records(st->dev, LOG_FILE, 1, 1, &count);
...
if (a->target == MOGBUS_TARGET_FIFO)
{
	mogbus_device_set_fifo(dev, (uint16_t)a->address, (uint16_t)st->sample_count, st->samples);
	return 0;
}
```

```python
def event(self, code):
    log = self.dev.read_file_records(EVENT_FILE, 0, EVENT_RECORDS)
    self.dev.write_file_records(EVENT_FILE, 0, log[1:] + [code])
    if code in (EV_HIGH_ON, EV_LOW_ON):
        self.alarms = (self.alarms + [code])[-31:]
        self.dev.set_fifo(ALARM_FIFO, self.alarms)
```

The C self-test reads both stores:

```text
  ok   FC 14 reads the event counter (at least 5 events)
  ok   FC 18 returns the last 31 level samples
```

In the Python program the operator switches to manual mode and leaves the pump off: the tank fills until
the high-level alarm, then the pump is started and the tank empties until the low-level alarm. The
client reads both stores with its `file_records()` and `fifo()` methods:

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
> The Python example's hook does not refuse them; add a rule on `access.target ==
> mogbus.AccessTarget.FILE_RECORD` for a log that only the host may write (the C hook does).

## A vendor function code

Some devices answer function codes the standard does not define, such as the vendor codes of commercial
PLCs. The station answers 0x41, "read station status block", with a block computed from the live values
when the request arrives. In a config, `(function-code ...)` matches the request data against patterns
and replies with fixed bytes, live registers or an echo (this one answers `01` with the first four
input registers: level and outflow); in C and Python a callback computes the reply, which is
how the block carries the mode, the alarm flags and the hook's refusal count.

```scheme
(function-code #x41
  (name "Station status")
  (on "01" (registers input 0 4))
  (on "02" (echo)))
```

```c
if (mogbus_device_add_function_code(st.dev, FC_STATUS_BLOCK, status_block, &st) != MOGBUS_OK)
```

```python
def status(request):
    if request != b"\x01":
        return 3
    flags = process.high | (process.low << 1) | (process.pump << 2)
    return struct.pack(">BBBHHH", 1, int(dev.get_value("holding", MODE)), flags,
                       round(dev.get_value("input", LEVEL) * 100),
                       round(dev.get_value("input", OUTFLOW) * 10), policy.refused)

dev.add_function_code(FC_STATUS_BLOCK, status)
```

The config snippet goes inside the `(device ...)` form (not part of the tutorial file; the
[vendor-function-codes profile](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/vendor-function-codes.scm)
has more). The C callback `status_block` gets the request data (one byte, the block id) and writes the
reply data; it returns 0 to reply, a Modbus exception code (an unknown block id answers 3) or
`MOGBUS_FUNCTION_NO_RESPONSE` for silence. The Python callback returns bytes (at most 252), an int
(an exception code) or `None` (no response). The client side is one call, `call(pdu)` in the Python
example, which sends a raw request and returns the reply:

```python
block = plc.call(struct.pack(">BB", FC_STATUS_BLOCK, 1))
```

```text
  ok   FC 0x41 returns the status block computed from the live values
  ok   FC 0x41 with an unknown block id: exception 03
```

The request goes through the same path as any other: faults, listen-only, counters and metrics apply.
The rules (codes 1-127 that are not built in, 8 per device) are in [user-defined function
codes](../modbus/user-defined-function-codes.md) and
[`function-code`](../config/device/function-code.md).

## Watching traffic

Two more hooks are library-only and not part of the tutorial program: a *frame hook* per server shows
every frame a server receives and sends and lets a test rewrite or drop them (a way to test a client
against corrupt answers that fixed faults cannot reach), and a *connection hook* tells the host when a
client connects or leaves. They are described, with examples, in [hooks in C](../library/library-hooks.md#watching-and-rewriting-frames)
and [hooks in Python](../python/python-hooks.md#watching-and-rewriting-frames); `mogbus --decode` and
[`mogbus_decode_frame()`](../modbus/decoding-frames.md) turn the bytes into text.

## Next

- [Step 5: Break it on purpose](tutorial-faults.md)
