# Step 4: Guard clients and keep records

> **Summary:** Install an access hook that vets client writes, react after accepted writes, log events in file records, and serve a FIFO that is refreshed just before a client reads it.

An access hook (@ref mogbus_access_fn "mogbus_access_fn") runs on the Modbus client path of one device. It is the embedded counterpart of subclassing a datastore: refresh a value right before a read, react to a write, or veto it. Install it with @ref mogbus_device_set_access_hook "mogbus_device_set_access_hook()" (NULL removes it; a device has one hook and a new one replaces it):

In the tutorial program, `main()` installs its function `on_access` on the station's device, passing the address of its `struct station` (see [step 1](library-device.md#the-programs-state)) as the `user` pointer, which the library hands back to the hook on every call:

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

The excerpts below are from `on_access`, where `a` is the `mogbus_access_t` of the request, `dev` the device, `st` the station struct recovered from `user`, and `float_of()` a helper that decodes two registers as a float32. The first rule reads the current mode with the host-side `mogbus_device_get_value()` (a host call, so it does not trigger the hook again) and refuses any write but a single write to the mode register while the station is off; the second decodes the float32 the client is writing and refuses it outside the range:

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
if (a->address <= HOLD_MODE && end > HOLD_MODE)
	log_event(st, EVT_MODE | a->values[HOLD_MODE - a->address]);
```

In the tutorial program `main()` installs `on_written` the same way as the access hook (`mogbus_device_set_written_hook(st.dev, on_written, &st)`). In the excerpt, `a` is the access describing the write that was applied, `st` the station struct and `end` the first address after the written range; the program writes a line to the event log (below) when the range covers the mode register. It gets the same @ref mogbus_access_t "mogbus_access_t" as the access hook, with `kind` always `MOGBUS_ACCESS_WRITE` and `values` the registers that were stored (bits as 0 or 1; for FC 16 the masked result). It runs once per written range: FC 05, 06, 0F, 10, 16, the write half of FC 17 (before the read half is answered) and, once per sub-request in order, FC 15 file records (`MOGBUS_TARGET_FILE_RECORD`). With `broadcast-execute` on it runs once per device.

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

`FC_STATUS_BLOCK` is the program's constant for 0x41, `status_block` its callback and `&st` the `user` pointer again. `status_block` gets the request data (one byte, the block id) and writes the reply data: block id, mode, flags (high, low, running), level in cm, outflow in 0.1 m3/h and the hook's refusal count, read with `mogbus_device_get_value()` and `mogbus_device_read()`. It returns 0 to reply, a Modbus exception code to refuse (an unknown block id answers 3) or `MOGBUS_FUNCTION_NO_RESPONSE` for silence. The request counts in the diagnostic counters and metrics like any other, and faults and listen-only apply. See [user-defined function codes](../modbus/user-defined-function-codes.md#programmable-from-c-and-python) for the rules (1-127, not built in, 8 per device).

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

Clients read the event log with FC 14; the host appends to it. The layout is the program's own: record 1 counts the events, records 2 to 100 are a ring of codes (high byte what, `EVT_*` constants such as `EVT_PUMP` and `EVT_MODE`; low byte the value). The program's `log_event(st, code)`, called by `simulate()` and by the post-write hook, appends one code: it picks the next slot of the ring from the event counter `st->events` and writes with @ref mogbus_device_write_file_records "mogbus_device_write_file_records()", a host call that does not trigger the hook:

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

FC 18 returns a queue of up to 31 values. `simulate()` keeps the last 31 level samples (in centimetres, newest last) in `st->samples`, `st->sample_count` of them; the first branch of `on_access` copies them into the queue right before a client reads it (`a->address` is the queue's pointer address):

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

## Watching and rewriting frames

The hooks above work on requests the device understood. A *frame hook* (@ref mogbus_frame_fn "mogbus_frame_fn", installed per server with @ref mogbus_server_set_frame_hook "mogbus_server_set_frame_hook()") sees the transport frames themselves, so a host can log traffic, or rewrite and drop frames to test a client against corrupt or missing answers; fixed fault injection covers only fixed cases. It is the counterpart of pymodbus `trace_packet`. It works on every transport: TCP (also with TLS, as plaintext), UDP, RTU (PTY, device, fd), RTU over TCP and UDP, ASCII (PTY, device, fd), ASCII over TCP and UDP.

This example is not part of the tutorial program. The hook below prints every frame and breaks the CRC of every RTU-over-TCP answer (it uses the `mogbus_frame_t` described after it):

```c
static size_t
trace(void* user, mogbus_server_t* server, mogbus_frame_t* f)
{
	(void)user;
	(void)server;
	printf("%s %s %zu bytes\n", f->direction == MOGBUS_FRAME_RECEIVED ? "rx" : "tx", f->peer, f->len);
	if (f->direction == MOGBUS_FRAME_SENT && f->mode == MOGBUS_MODE_RTU_TCP)
		f->data[f->len - 1] ^= 0xFF;	/* break the CRC of every answer */
	return f->len;
}

mogbus_server_set_frame_hook(server, trace, NULL);
```

What the hook sees, as `mogbus_frame_t` (`direction`, `mode`, `peer`, `data`, `len`, `capacity`):

- **Received**: once per complete frame as the transport delimits it, before any validation or decoding. That is the MBAP ADU (7-byte header plus PDU) for TCP and UDP (for UDP every datagram that is not empty or oversized, well formed or not), the RTU frame with unit id and CRC for the RTU modes, and the ASCII frame from the colon through CR LF for the ASCII modes. On RTU byte streams and RTU over TCP the CRC is what delimits a frame, so only frames with a valid CRC reach the hook (the hook can still break it); an RTU-over-UDP datagram is passed whole. Bytes the framer throws away before a frame exists (resynchronization garbage, partial-frame timeouts, CR, LF and spaces between ASCII frames) are not shown.
- **Sent**: once per complete encoded response, after fault injection (a `corrupt` fault is already applied) and just before it is written or queued. With a `delay` fault the hook runs when the response is built, not when the delayed bytes leave. Nothing is shown when there is no response (broadcast, `drop` fault, listen-only, bad CRC, unknown unit).
- `peer` is the client text of the request log line (`ip:port`); it is `""` on a serial byte stream. `data` holds `len` bytes and has room for `capacity`: 260 for MBAP, 256 for RTU and 513 for ASCII, so a rewrite may lengthen a frame up to that.

The return value is the new length of the frame:

- The length it was given (or another length up to `capacity`) lets the frame go on. A received frame is then validated and decoded exactly as if the rewritten bytes had arrived: the hook can corrupt a CRC or LRC, change a register address, change the unit id. On TCP a rewritten MBAP header that no longer fits the frame is ignored and counted as rejected; the connection stays open.
- 0 drops the frame. A dropped received frame is treated as never received: no processing, no counters, no response. A dropped response is not sent; the TCP connection stays open and the metrics already recorded for the request are unchanged.
- A value above `capacity` drops the frame like 0 and logs a warning.

The hook runs on the thread that calls @ref mogbus_server_step "mogbus_server_step()" or @ref mogbus_server_run "mogbus_server_run()", must not block, and must not close or step the server; it may call `mogbus_device_*`. The frame and `peer` are valid only during the call. A server has one hook, a new one replaces it and NULL removes it. It is not part of a config, and HTTP endpoint traffic is not shown.

To read what the hook sees, @ref mogbus_decode_frame "mogbus_decode_frame()" turns the bytes of a frame into text or JSON (every field of the ADU and the PDU, with the checksum verdict), without a context or a server; it is the library side of `mogbus --decode` ([Decoding frames](../modbus/decoding-frames.md)). It only reads its input, so the hook may call it on `f->data`:

```c
char text[MOGBUS_DECODE_BUFFER_SIZE];
size_t len;

mogbus_decode_frame(f->data, f->len, MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO, false, text, sizeof(text), &len);
puts(text);
```

It returns `MOGBUS_OK`, or `MOGBUS_ERR_FRAME` for a damaged frame (the text is complete and lists the problems), `MOGBUS_ERR_NOMEM` when the buffer is too small (`MOGBUS_DECODE_BUFFER_SIZE` always suffices) and `MOGBUS_ERR_INVALID` for NULL arguments or a frame above 4096 bytes.

## Watching connections

A *connection hook* (@ref mogbus_connection_fn "mogbus_connection_fn", installed per server with @ref mogbus_server_set_connection_hook "mogbus_server_set_connection_hook()") tells the host when a client connects to or leaves a server. It is the counterpart of pymodbus `trace_connect`:

```c
static void
on_connection(void* user, mogbus_server_t* server, const mogbus_connection_t* c)
{
	(void)user;
	(void)server;
	if (c->connected)
		printf("#%llu %s connected\n", (unsigned long long)c->id, c->peer);
	else
		printf("#%llu %s left (reason %d)\n", (unsigned long long)c->id, c->peer, c->reason);
}

mogbus_server_set_connection_hook(server, on_connection, NULL);
```

`mogbus_connection_t` holds `connected`, `id`, `mode`, `peer` and `reason`:

- It works for the connection-oriented transports `MOGBUS_MODE_TCP` (also with TLS), `MOGBUS_MODE_RTU_TCP` and `MOGBUS_MODE_ASCII_TCP`. UDP, serial, PTY and fd servers have no connections: setting the hook there succeeds (so a host can treat every server the same) and it never fires.
- **Open**: sent when the server accepts the socket, before any data and before a TLS handshake. A client refused because all 32 slots are in use is never accepted and is not reported.
- **Close**: sent exactly once for every connection that was announced, with `reason`: `MOGBUS_CLOSE_PEER` (end of stream or hang-up), `MOGBUS_CLOSE_IDLE_TIMEOUT`, `MOGBUS_CLOSE_DISCONNECT_FAULT`, `MOGBUS_CLOSE_TLS` (TLS setup failure, failed handshake or handshake timeout; the open event came first), `MOGBUS_CLOSE_SERVER_STOP` (the server or context was closed with the client connected, including at the end of @ref mogbus_run "mogbus_run()") or `MOGBUS_CLOSE_ERROR` (socket error, bad MBAP header, full receive buffer). Connections already open when the hook is installed are not announced and produce no close event.
- `id` is the same in the open and the close event and is never reused by the server, so a reused connection slot gets a new id. `peer` is `ip:port`, as in the log lines and in `mogbus_frame_t.peer`, and is valid only during the call.

The hook runs with the context lock held on the thread that steps or closes the server, must not block, and must not close or step the server; it may call `mogbus_*` functions. A server has one hook, a new one replaces it and NULL removes it. It is not part of a config.

## Next

- [Step 5: Observe, shut down, lifetimes and threading](library-lifetimes.md)
