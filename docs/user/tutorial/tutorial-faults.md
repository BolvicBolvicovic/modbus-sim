# Step 5: Break it on purpose

> **Summary:** Make the station slow, silent or wrong the way a real PLC sometimes is: faults in the config, scripted failures that repeat on every run, and the HTTP control API that sets faults and sensor values from a test while the server runs.

A client that only ever sees a perfect device is not tested. mogbus injects faults: late answers,
dropped answers, exceptions, corrupted frames, disconnects, restarts and a failing sensor. A fault never
changes how a request is processed: it acts on the answer. You can set a fault in two ways:

- In a **config file**. A C or Python program gets these faults by loading the file with `mogbus_load_config()` / `Context.load_config()`; the C and Python calls that build a device in code cannot define faults.
- At **run time**, through the HTTP control API, from any program: a shell with `curl`, a Python script, your own test. This works the same for a server started by the `mogbus` program, by a C program or by a Python script.

## A fault in the config

A fault is part of a device, one per kind. This one makes 2 percent of the answers arrive 800 ms late.
It goes inside the `(device ...)` form of your config, after the machines (the file
[`pump-station.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/tutorial/pump-station.scm)
has it):

```scheme
  ; One answer in fifty arrives 800 ms late, like a busy PLC.
  (fault (kind delay) (delay-ms 800) (percent 2))
```

A delayed answer does not block the other clients. A random fault like this one is different on every
run. For a test that must fail the same way every time, use a [schedule](../config/faults/schedule.md)
(up to 8 steps, not repeating). This makes the sensors fail five seconds after the start, for the next 100
requests:

```scheme
  (fault (kind sensor-disconnect)
    (schedule (step (after-ms 5000) (count 100))))
```

[`sensor-disconnect`](../config/faults/sensor-disconnect.md) answers reads of input registers and
discrete inputs with exception 04 while it fires, then they work again. All the kinds are in the [fault
reference](../config/faults/index.md).

## Change it while it runs

The [control API](../observability/api/control-api.md) changes the running server from outside. It has no
password, so it is off by default: you switch it on with the HTTP endpoint, and it should stay on
`127.0.0.1`.

For the Scheme / Bash route, add the endpoint to the `(server ...)` form of the config (the full file has
it) and start `./mogbus pump-station.scm`. For C and Python, set the options of the server:

```scheme
(server (mode tcp) (port 1502) (seed 7)
  (http (port 9102) (control on)))
```

```c
/* control.c - serve the pump station with the HTTP control API switched on. */
#include <stdio.h>
#include "mogbus.h"

int
main(void)
{
	mogbus_t* ctx = NULL;
	mogbus_server_t* server = NULL;

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

	mogbus_server_options_t options = mogbus_server_options_defaults();

	/* The HTTP endpoint on 127.0.0.1:9102 with the control API under /api/. The control API has
	 * no password: keep it on 127.0.0.1. */
	options.http_bind_address = "127.0.0.1";
	options.http_port = 9102;
	options.http_control = true;

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
# control.py - serve the pump station, then break it from the outside with the HTTP control API.
import urllib.request
import mogbus

BASE = "http://127.0.0.1:9102"  # the HTTP endpoint of the server below


def control(method, path, body=None):
    """Sends one request to the control API and returns the answer as text."""
    data = body.encode() if body else None
    request = urllib.request.Request(BASE + path, data=data, method=method)
    with urllib.request.urlopen(request, timeout=2) as answer:
        return answer.read().decode()


with mogbus.Context() as ctx:
    ctx.load_config("station.scm")

    # The HTTP endpoint on 127.0.0.1:9102 with the control API under /api/. The control API
    # has no password: keep it on 127.0.0.1.
    options = mogbus.ServerOptions(port=1502, http_bind_address="127.0.0.1", http_port=9102,
                                   http_control=True)

    with ctx.serve(options).start():
        with mogbus.Client(port=1502, unit=1) as plc:
            # Make every answer an exception 06 (server device busy).
            control("PUT", "/api/devices/1/faults/exception", '{"exception_code": 6, "percent": 100}')
            try:
                plc.read_holding_registers(0, 2)
            except mogbus.ModbusException as exc:
                print("with the fault: exception", exc.code)

            # Clear the fault: the station answers normally again.
            control("DELETE", "/api/devices/1/faults/exception")
            print("without the fault:", plc.read_holding_registers(0, 2))
```

The Python script uses the control API itself, with `urllib`, and prints:

```text
with the fault: exception 6
without the fault: [16416, 0]
```

From a shell, `curl` does the same for any of the three servers (start the server of your route first, the
C program is `control.c`). List the devices, then play the physical side: a
[value write](../observability/api/api-set-value.md) sets the decoded number of a point, like a sensor on
the host side.

```bash
curl -s localhost:9102/api/devices
curl -s -X PUT -d '{"value":4.5}' localhost:9102/api/devices/1/input/0/value
./tools/modbus_probe.py input 0 2 --as float32
```

```text
[{"unit":1,"name":"Pump station","points":{"coil":1,"discrete":3,"input":6,"holding":3}}]
{"ok":true}
[4.5]
```

(With `pump-station.scm` of step 3 the high-level alarm turns on, the `pump` machine starts the pump and the
level falls again from 4.5; with `station.scm` of step 1 it stays at 4.5.) Faults are set and cleared the same way ([set](../observability/api/api-set-fault.md),
[clear](../observability/api/api-clear-fault.md)). Here the test makes every answer exception 06 (server
device busy), to find out what its client does, then clears the fault:

```bash
curl -s -X PUT -d '{"exception_code":6,"percent":100}' localhost:9102/api/devices/1/faults/exception
./tools/modbus_probe.py holding 2 1
curl -s -X DELETE localhost:9102/api/devices/1/faults/exception
./tools/modbus_probe.py holding 2 1
```

```text
{"ok":true}
error: exception 0x06: server device busy
{"ok":true}
[1]
```

The same works for `delay`, `drop`, `corrupt` and the other kinds. See also [using the control API from
Python](../python/python-lifetimes.md#runtime-control).

## Next

- [Step 6: Observe it](tutorial-observe.md)
