# Step 4: Guard clients and keep records

> **Summary:** Make the station refuse what the real PLC would refuse, keep an event log and an alarm history that clients can read, and answer a vendor function code, with hooks in C and Python and with fixed config keys where the config can do it.

The real controller does not accept every write, and it keeps records a data logger reads. Two kinds of
*hooks*, functions of your program that mogbus calls on a Modbus client request, do this in C and Python:

- The *access hook* runs before a client request touches the device. It returns 0 to let the request through, or a Modbus exception code to refuse it. A refused write changes nothing.
- The *written hook* runs after a client write was accepted and stored.

Hooks see only what a Modbus client does. Your own `set_value` and `write` calls (step 3) never call them,
so a hook can change the device without calling itself again.

The config has no hooks. It has the fixed equivalents mentioned below: [`(access read-only)`](../config/points/access.md)
to refuse every write to a point, `file` and `fifo` data (step 1) and `function-code`.

## Refuse writes and record them

The station of this step has two rules and two records:

- A write of the mode (holding 2) with a value above 2 is refused with exception 03 (illegal data value).
- After an accepted write of the mode, the event log (file 1) holds the number of mode changes in record 0 and the latest mode in record 1, and the alarm history (FIFO 0) holds the last modes, up to 31 of them.

The programs below load `station.scm` of step 1 and then play both the station and a client that tries it.
For the C route the client is a second program, `try_guard.c`.

```c
/* guard.c - serve the pump station and guard what clients write to it. */
#include <stdio.h>
#include <string.h>
#include "mogbus.h"

/* The last modes a client wrote, kept by the program and handed to the hook as `user`. */
struct mode_history
{
	uint16_t modes[31]; /* oldest first; a FIFO holds at most 31 values */
	size_t count;       /* how many of them are used */
};

/* Runs before every write a Modbus client makes to the device. Return 0 to let the request
 * through, or a Modbus exception code to refuse it (nothing is written then). */
static uint8_t
check_write(void* user, mogbus_device_t* dev, const mogbus_access_t* access)
{
	(void)user;
	(void)dev;

	/* Only writes to the holding registers are checked. */
	if (access->kind != MOGBUS_ACCESS_WRITE
	    || access->target != MOGBUS_TARGET_TABLE
	    || access->space != MOGBUS_HOLDING)
		return 0;

	/* Does the write cover holding register 2, the mode? `values` are the registers about to be
	 * stored, starting at access->address. */
	if (access->address <= 2 && access->address + access->count > 2)
	{
		uint16_t mode = access->values[2 - access->address];

		if (mode > 2)
			return 3; /* exception 03, illegal data value: the modes are 0, 1 and 2 */
	}
	return 0;
}

/* Runs after a client write was accepted and stored. */
static void
record_mode_change(void* user, mogbus_device_t* dev, const mogbus_access_t* access)
{
	struct mode_history* history = user;

	/* Only a write that covers holding register 2, the mode, is recorded. */
	if (access->target != MOGBUS_TARGET_TABLE
	    || access->space != MOGBUS_HOLDING
	    || access->address > 2
	    || access->address + access->count <= 2)
		return;

	uint16_t mode = access->values[2 - access->address];

	/* Event log, file 1: record 0 counts the changes, record 1 is the latest mode. */
	uint16_t event_log[2] = { 0, 0 };

	mogbus_device_read_file_records(dev, 1, 0, 2, event_log);
	event_log[0] += 1;
	event_log[1] = mode;
	mogbus_device_write_file_records(dev, 1, 0, 2, event_log);

	/* Alarm history, FIFO 0: the last 31 modes, oldest first. */
	if (history->count == 31)
	{
		memmove(history->modes, history->modes + 1, 30 * sizeof history->modes[0]);
		history->count = 30;
	}
	history->modes[history->count] = mode;
	history->count += 1;
	mogbus_device_set_fifo(dev, 0, (uint16_t)history->count, history->modes);
}

int
main(void)
{
	mogbus_t* ctx = NULL;
	mogbus_server_t* server = NULL;
	struct mode_history history = { .count = 0 };

	if (mogbus_create(&ctx) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(NULL));
		return 1;
	}
	if (mogbus_load_config(ctx, "station.scm") != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	mogbus_device_t* dev = mogbus_find_device(ctx, 1); /* the device with unit id 1 */

	if (mogbus_device_set_access_hook(dev, check_write, NULL) != MOGBUS_OK
	    || mogbus_device_set_written_hook(dev, record_mode_change, &history) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	mogbus_server_options_t options = mogbus_server_options_defaults();

	if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
	{
	}

	mogbus_destroy(ctx);
	return 0;
}
```

```python
# guard.py - serve the pump station, guard what clients write to it, then try it as a client.
import mogbus

with mogbus.Context() as ctx:
    ctx.load_config("station.scm")
    dev = ctx.find_device(1)  # the device with unit id 1
    history = []              # the last modes a client wrote, oldest first

    def check_write(access):
        """Runs before every write a Modbus client makes. Return None to let it through, or a
        Modbus exception code to refuse it (nothing is written then)."""
        # Only writes to the holding registers are checked.
        if access.kind != mogbus.AccessKind.WRITE or access.space != mogbus.Space.HOLDING:
            return None
        # Does the write cover holding register 2, the mode? access.values are the registers
        # about to be stored, starting at access.address.
        if access.address <= 2 < access.address + access.count:
            mode = access.values[2 - access.address]
            if mode > 2:
                return 3  # exception 03, illegal data value: the modes are 0, 1 and 2
        return None

    def record_mode_change(access):
        """Runs after a client write was accepted and stored."""
        # Only a write that covers holding register 2, the mode, is recorded.
        if access.space != mogbus.Space.HOLDING or not access.address <= 2 < access.address + access.count:
            return
        mode = access.values[2 - access.address]

        # Event log, file 1: record 0 counts the changes, record 1 is the latest mode.
        changes = dev.read_file_records(1, 0, 1)[0] + 1
        dev.write_file_records(1, 0, [changes, mode])

        # Alarm history, FIFO 0: the last 31 modes, oldest first.
        history.append(mode)
        dev.set_fifo(0, history[-31:])

    dev.set_access_hook(check_write)
    dev.set_written_hook(record_mode_change)

    with ctx.serve(mogbus.ServerOptions(port=1502)).start():
        with mogbus.Client(port=1502, unit=1) as plc:
            # A mode that does not exist is refused with exception 03 ...
            try:
                plc.write_register(2, 7)
            except mogbus.ModbusException as exc:
                print("mode 7 refused: exception", exc.code)

            # ... and a valid one is accepted, then recorded.
            plc.write_register(2, 2)
            plc.write_register(2, 1)

            # File 1, records 0 and 1: (file, first record, number of records).
            print("event log:", plc.read_file_records([(1, 0, 2)])[0])
            print("alarm history:", plc.read_fifo(0))
```

- **Hook arguments.** The access passed to a hook is a @ref mogbus_access_t "mogbus_access_t" (Python \ref mogbus::core::Access "Access"): the `kind` (read or write), the `target` (a table, a file record, a FIFO), the table `space`, the first `address`, the `count` and, for a write, the `values` that are about to be stored, starting at that address. A write of several registers can cover the mode register without starting there, which is why the hooks compute `2 - address`.
- **C.** @ref mogbus_device_set_access_hook "mogbus_device_set_access_hook()" and @ref mogbus_device_set_written_hook "mogbus_device_set_written_hook()" take your function and one pointer, `user`, that mogbus hands back on every call; the program passes its `history` structure that way. The hook returns `uint8_t`: 0 or an exception code.
- **Python.** \ref mogbus::core::Device::set_access_hook "Device.set_access_hook()" and \ref mogbus::core::Device::set_written_hook "Device.set_written_hook()" take any function that accepts an `Access`. The access hook returns `None` or an exception code.
- **Host side.** The hooks write the records with @ref mogbus_device_write_file_records "mogbus_device_write_file_records()" and @ref mogbus_device_set_fifo "mogbus_device_set_fifo()" (Python `write_file_records()` and `set_fifo()`). The host fills the stores; a client only reads them, with function codes 0x14 and 0x18.

A hook runs on the thread that serves, with the context locked, so keep it short. An undefined address, a
read-only point or a point that a machine drives answers exception 02 without calling the hook. More,
including the hooks for file records and FIFO reads: [hooks in C](../library/library-hooks.md) and [hooks in
Python](../python/python-hooks.md).

## Try it

The Python script is its own client and prints:

```text
mode 7 refused: exception 3
event log: [2, 1]
alarm history: [2, 1]
```

For the C route, build `guard.c` and `try_guard.c`, start `./guard` in one terminal and run this client in
another:

```c
/* try_guard.c - a Modbus client that tries the rules of guard.c. */
#include <stdio.h>
#include "mogbus.h"

int
main(void)
{
	mogbus_client_t* plc = NULL;
	mogbus_client_options_t options = mogbus_client_options_defaults(); /* TCP to 127.0.0.1:1502 */

	if (mogbus_client_open(&options, &plc) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(NULL));
		return 1;
	}

	/* A mode that does not exist is refused with exception 03 ... */
	if (mogbus_client_write_register(plc, 1, 2, 7) != MOGBUS_OK)
		printf("mode 7 refused: exception %u\n", (unsigned)mogbus_client_last_exception(plc));

	/* ... and a valid one is accepted, then recorded. */
	mogbus_client_write_register(plc, 1, 2, 2);
	mogbus_client_write_register(plc, 1, 2, 1);

	/* File 1, records 0 and 1: the event log. */
	uint16_t event_log[2] = { 0, 0 };
	mogbus_client_file_record_t request = { .file = 1, .record = 0, .length = 2, .values = event_log };

	if (mogbus_client_read_file_records(plc, 1, &request, 1) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_client_last_error(plc));
		return 1;
	}
	printf("event log: %u changes, latest mode %u\n", (unsigned)event_log[0], (unsigned)event_log[1]);

	/* FIFO 0: the alarm history. */
	uint16_t history[31];
	size_t history_count = 0;

	if (mogbus_client_read_fifo(plc, 1, 0, history, 31, &history_count) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_client_last_error(plc));
		return 1;
	}
	printf("alarm history:");
	for (size_t i = 0; i < history_count; i++)
		printf(" %u", (unsigned)history[i]);
	printf("\n");

	mogbus_client_close(plc);
	return 0;
}
```

```text
mode 7 refused: exception 3
event log: 2 changes, latest mode 1
alarm history: 2 1
```

The refused write of 7 is not counted: only the two accepted writes (mode 2, then mode 1) are in the log
and in the history. Reading file records and FIFO queues from a client is the same on every route; with
the `modbus_probe.py` tool you can see the refusal (`./tools/modbus_probe.py write-reg 2 7` prints `error:
exception 0x03: illegal data value`).

## A vendor function code

Some devices answer function codes the standard does not define. This station answers 0x41, "read station
status": the request has one data byte, the block number 1, and the reply is the mode followed by 1 if the
pump runs. In a config, `(function-code ...)` matches the request data against patterns and replies with
fixed bytes, live registers or an echo; C and Python compute the reply in a function.

```scheme
(function-code #x41
  (name "Station status")
  (on "01" (registers input 0 2))   ; block 1: reply with the two input registers 0 and 1, the level
  (on "02" (echo)))                 ; block 2: reply with the request data
```

```c
/* Function code 0x41, "read station status": the request data is one byte, the block number 1.
 * The reply is two bytes: the mode, then 1 if the pump runs and 0 if not. */
static int
status_block(void* user, mogbus_device_t* dev, uint8_t unit_id, uint8_t function_code,
	     const uint8_t* data, size_t length, uint8_t* reply, size_t reply_capacity,
	     size_t* reply_length)
{
	(void)user;
	(void)unit_id;
	(void)function_code;
	(void)reply_capacity;

	if (length != 1 || data[0] != 1)
		return 3; /* exception 03: only block 1 exists */

	double mode = 0;
	double running = 0;

	mogbus_device_get_value(dev, MOGBUS_HOLDING, 2, &mode);
	mogbus_device_get_value(dev, MOGBUS_DISCRETE, 2, &running);
	reply[0] = (uint8_t)mode;
	reply[1] = running != 0 ? 1 : 0;
	*reply_length = 2;
	return 0; /* 0 means "send the reply"; another value is the exception code to answer */
}
```

```python
    def status_block(request):
        """Function code 0x41, "read station status": the request data is one byte, the block
        number 1. The reply is two bytes: the mode, then 1 if the pump runs and 0 if not."""
        if request != b"\x01":
            return 3  # exception 03: only block 1 exists
        mode = int(dev.get_value("holding", 2))
        running = int(dev.get_value("discrete", 2))
        return bytes([mode, running])  # bytes are the reply; an int is the exception code to answer

    dev.add_function_code(0x41, status_block)
```

The Scheme form goes inside the `(device ...)` form. The C function goes above `main()` of `guard.c`, and
this call goes into `main()` of `guard.c` after `dev` is found and before the server is created:

```c
	if (mogbus_device_add_function_code(dev, 0x41, status_block, NULL) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}
```

The Python function and its `add_function_code()` call go into `guard.py` before `dev.set_access_hook(check_write)`.
The client side is a raw request: in `try_guard.c`, before `mogbus_client_close(plc)`:

```c
	/* Ask for status block 1 with function code 0x41; the reply is the mode, then the pump. */
	uint8_t block_number[1] = { 1 };
	uint8_t status[2] = { 0, 0 };
	mogbus_request_t status_request = {
		.unit = 1, .function_code = 0x41,
		.data = block_number, .data_len = 1,
		.reply = status, .reply_cap = sizeof status,
	};

	if (mogbus_client_request(plc, &status_request) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_client_last_error(plc));
		return 1;
	}
	printf("status block: mode %u, pump %u\n", (unsigned)status[0], (unsigned)status[1]);
```

and in `guard.py`, after the last `print`:

```python
            # Ask for status block 1 with function code 0x41; the reply is the mode, then the pump.
            reply = plc.request(0x41, b"\x01")
            print("status block:", list(reply))
```

```text
status block: mode 1, pump 0
```

(The Python script prints `status block: [1, 0]`.) The request goes through the same path as any other:
faults, counters and metrics apply. A callback returns 0 (or, in Python, bytes) to answer, an exception
code to refuse, and `MOGBUS_FUNCTION_NO_RESPONSE` (Python `None`) to stay silent. The rules are in
[user-defined function codes](../modbus/user-defined-function-codes.md) and
[`function-code`](../config/device/function-code.md).

## Watching traffic

Two more hooks are only in the library: a *frame hook* per server shows every frame the server receives and
sends and lets a test rewrite or drop them, and a *connection hook* tells you when a client connects or
leaves. See [hooks in C](../library/library-hooks.md#watching-and-rewriting-frames) and [hooks in
Python](../python/python-hooks.md#watching-and-rewriting-frames). `mogbus --decode` and
[`mogbus_decode_frame()`](../modbus/decoding-frames.md) turn captured bytes into text.

## Next

- [Step 5: Break it on purpose](tutorial-faults.md)
