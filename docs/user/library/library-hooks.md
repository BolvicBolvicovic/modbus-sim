# Step 4: Guard clients and keep records

Install an access hook that vets client writes, log events in file records, and serve a FIFO that is refreshed just before a client reads it.

An access hook (@ref mogbus_access_fn "mogbus_access_fn") runs on the Modbus client path of one device. It is the embedded counterpart of subclassing a datastore: refresh a value right before a read, react to a write, or veto it. Install it with @ref mogbus_device_set_access_hook "mogbus_device_set_access_hook()" (NULL removes it; a device has one hook and a new one replaces it):

```c
mogbus_device_set_access_hook(st.dev, on_access, &st);
```

The hook gets the `user` pointer, the device and a @ref mogbus_access_t "mogbus_access_t" describing the request: `kind` (read or write), the `target`, the `function_code` and the fields the target uses. For the four tables those are `space`, the first `address`, the `count` of registers or bits and, for writes, the raw `values` about to be stored (bits as 0 or 1; NULL for reads). Return 0 to let the request go on, or a Modbus exception code to answer instead. A vetoed write changes nothing, so [writes stay all-or-nothing](../modbus/writes.md).

## Rules for the station

`on_access` in the example enforces what the real PLC would:

- while the mode is off, every write but the mode register is refused with exception 04 (server device failure);
- a setpoint outside 0.5 to 4.5 m, or half of the float32, is refused with exception 03;
- a mode above 2 is refused with exception 03;
- the event log is the host's: client writes to it get exception 01.

```c
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

A client may write a range that overlaps the setpoint partly (an FC 16 over holding 1-2), so the hook checks both registers of the float are in the range or none. The self-test tries each rule:

```text
  ok   FC 16 setpoint 9.0 m is refused by the hook: exception 03
  client write: FC 10, holding address 0, 2 item(s)
  ok   FC 16 setpoint 3.0 m is accepted
  ok   FC 06 mode 7 is refused: exception 03
  ok   FC 05 while off is refused: exception 04
  ok   FC 15 on the event log is refused: exception 01
```

The `client write:` line is the post-write hook (see below) logging every accepted write: a data logger under test can be checked against it. Calls from the host side (`mogbus_device_write`, `set_value`, the HTTP control API, machines) never reach the hook.

## Where the hook runs

- Reads (FC 01, 02, 03, 04, the read half of FC 17 and the `registers` action of [user-defined function codes](../modbus/user-defined-function-codes.md)): after the range is known to be defined and before the table is read, so the client receives what the hook stored.
- Writes (FC 05, 06, 0F, 10, 16 and the write half of FC 17): after the range is known to be defined and writable, before anything is stored. FC 16 reports the masked result that would be stored. FC 17 runs the write hook first, then (once the write is applied) the read hook; an exception from the read hook leaves the write applied.
- An undefined address, a read-only point or a machine-driven point answers exception 02 without calling the hook. That is why the example's read-only site name needs no rule in the hook.
- The exception counts like any other the simulator answers (diagnostic counter, metrics). With `broadcast-execute` on, the hook runs once per device, and faults still act on the response.
- Diagnostics (FC 08) and device identification (FC 2B/0E) are not covered.

The hook runs on the thread that steps the server, so it may call the other `mogbus_device_*` functions on the same device (the example calls `get_value`, `set_fifo` and the file-record writes), but it must not block: no other client is served meanwhile. The pointers in the access are valid only during the call. A hook is not part of a config and does not survive @ref mogbus_destroy "mogbus_destroy()".

## Reacting after a write

The access hook runs before a write, so it cannot tell that the write happened. A *post-write hook* (@ref mogbus_written_fn "mogbus_written_fn", installed with @ref mogbus_device_set_written_hook "mogbus_device_set_written_hook()") runs after a Modbus client write was applied: the embedded counterpart of pymodbus `setValues`. The example uses it to count and log accepted writes and to put setpoint and mode changes in the event log, instead of polling the registers every tick:

```c
mogbus_device_set_written_hook(st.dev, on_written, &st);
...
if (a->address <= HOLD_MODE && end > HOLD_MODE)
	log_event(st, EVT_MODE | a->values[HOLD_MODE - a->address]);
```

It gets the same @ref mogbus_access_t "mogbus_access_t" as the access hook, with `kind` always `MOGBUS_ACCESS_WRITE` and `values` the registers that were stored (bits as 0 or 1; for FC 16 the masked result). It runs once per written range: FC 05, 06, 0F, 10, 16, the write half of FC 17 (before the read half is answered) and, once per sub-request in order, FC 15 file records (`MOGBUS_TARGET_FILE_RECORD`). With `broadcast-execute` on it runs once per device.

- It does not run for a write the access hook vetoed, for a request answered with an exception, or for host calls (`mogbus_device_write`, `set_value`, the HTTP control API, machines). That is why `log_event` and the other `mogbus_device_*` calls it makes do not trigger it again.
- Faults do not change request processing: the hook runs even when the response is then dropped, delayed or corrupted, and a `restart` fault resets the device after the response is built, so the hook has already run.
- There is one per device, a new one replaces it and NULL removes it. It runs on the server thread, must not block and the `values` pointer is valid only during the call.

In the self-test, a refused setpoint leaves the event counter alone and an accepted one advances it by one:

```text
  ok   the refused write did not reach the post-write hook
  ok   the post-write hook logged the setpoint event
```

## A vendor function code

Some devices answer function codes the standard does not define. The pump station answers the vendor code 0x41, "read station status block", with a block computed from the live values at request time. @ref mogbus_device_add_function_code "mogbus_device_add_function_code()" defines it with a callback (@ref mogbus_function_fn "mogbus_function_fn"), the programmable counterpart of the config's `(function-code ...)`:

```c
mogbus_device_add_function_code(st.dev, FC_STATUS_BLOCK, status_block, &st);
```

`status_block` gets the request data (one byte, the block id) and writes the reply data: block id, mode, flags (high, low, running), level in cm, outflow in 0.1 m3/h and the hook's refusal count, read with `mogbus_device_get_value()` and `mogbus_device_read()`. It returns 0 to reply, a Modbus exception code to refuse (an unknown block id answers 3) or `MOGBUS_FUNCTION_NO_RESPONSE` for silence. The request counts in the diagnostic counters and metrics like any other, and faults and listen-only apply. See [user-defined function codes](../modbus/user-defined-function-codes.md#programmable-from-c-and-python) for the rules (1-127, not built in, 8 per device).

```text
  ok   FC 0x41 returns the status block computed from the live values
  ok   FC 0x41 with an unknown block id: exception 03
```

## Targets

`target` is `MOGBUS_TARGET_TABLE` (0) unless the request reaches another store, and the fields a target does not use are zero. Code that handles only tables must check it, because the other targets also reach hooks written before they existed; the example's hook starts with that dispatch.

| `target` | Function codes | Fields used |
|---|---|---|
| `MOGBUS_TARGET_TABLE` | 01-06, 0F, 10, 16, 17, user-defined `registers` | `space`, `address`, `count`, `values` (writes) |
| `MOGBUS_TARGET_FILE_RECORD` | 14, 15 | `file`, `address` (first record), `count` (records), `values` (writes) |
| `MOGBUS_TARGET_FIFO` | 18 | `address` (the queue's pointer address), reads only |
| `MOGBUS_TARGET_CANOPEN` | 2B/0D | `node`, `index`, `subindex`, `address` (byte offset), `count` (bytes), `data`/`data_len` (writes) |

- FC 14 (Read File Record): after the whole request is validated, the read hook runs once per sub-request in request order, then the response is built, so records the hook rewrote are what the client reads. A nonzero return answers that exception and later sub-requests are not offered to the hook.
- FC 15 (Write File Record): after validation, the write hook runs for every sub-request in order before anything is written; a nonzero return writes nothing.
- FC 18 (Read FIFO Queue): after the address is known to exist and before the queue is read.
- FC 2B/0D (CANopen): once the object exists and the range is inside it. A veto is answered as the extended failure `CE` with the SDO abort code `0x08000020`, whatever nonzero value is returned, because the CANopen response has no place for a Modbus exception code (see [CANopen General Reference](../modbus/fc2b-0d-canopen-general-reference.md)).

## An event log in file records

Clients read the event log with FC 14; the host appends to it. The layout is the program's own: record 1 counts the events, records 2 to 100 are a ring of codes (high byte what, low byte value). `log_event` writes with @ref mogbus_device_write_file_records "mogbus_device_write_file_records()", a host call that does not trigger the hook:

```c
uint16_t slot = (uint16_t)(2 + st->events % (LOG_RECORDS - 1));
uint16_t count = (uint16_t)++st->events;

mogbus_device_write_file_records(st->dev, LOG_FILE, slot, 1, &code);
mogbus_device_write_file_records(st->dev, LOG_FILE, 1, 1, &count);
```

@ref mogbus_device_read_file_records "mogbus_device_read_file_records()" reads them from the host. Both fail, changing nothing, if any record is undefined. The simulation logs pump starts and stops and alarms; the post-write hook logs accepted setpoint and mode writes (`EVT_SETPOINT | setpoint*10`, `EVT_MODE | mode`). The self-test's FC 14 request reads record 1:

```text
  ok   FC 14 reads the event counter (at least 5 events)
```

## A FIFO refreshed on read

FC 18 returns a queue of up to 31 values. The simulation keeps the last 31 level samples (in centimetres); the hook copies them into the queue right before a client reads it:

```c
if (a->target == MOGBUS_TARGET_FIFO)
{
	mogbus_device_set_fifo(dev, (uint16_t)a->address, (uint16_t)st->sample_count, st->samples);
	return 0;
}
```

@ref mogbus_device_set_fifo "mogbus_device_set_fifo()" replaces the queue's contents (0 to 31 values), host side. Refreshing in the hook means the queue costs nothing between reads.

```text
  ok   FC 18 returns the last 31 level samples
```

## CANopen objects

For FC 2B/0D, @ref mogbus_device_add_canopen_object "mogbus_device_add_canopen_object()" adds an object (node 1-127, index, subindex, 1-238 bytes), and @ref mogbus_device_read_canopen "mogbus_device_read_canopen()" and @ref mogbus_device_write_canopen "mogbus_device_write_canopen()" move bytes at an offset from the host. The pump station has none; the hook receives `MOGBUS_TARGET_CANOPEN` accesses with `node`, `index` and `subindex` when a client reads or writes one.

## Next

- [Step 5: Observe, shut down, lifetimes and threading](library-lifetimes.md)
