# Step 8: Stop it and ship it

> **Summary:** Stop the station cleanly on Ctrl-C or on a shutdown, let a second thread update it while the server runs, and run the config route in a container.

## Stopping

The `mogbus` program stops on SIGINT (Ctrl-C) and SIGTERM with exit status 0, which is what `docker stop`
needs. A program of your own installs no signal handlers unless you ask:

```bash
./mogbus pump-station.scm      # press Ctrl-C
```

```c
/* stop.c - serve the pump station until Ctrl-C or a SIGTERM, then shut down cleanly. */
#include <stdio.h>
#include "mogbus.h"

/* The signal handler of the library stops the server stored here. It must be a global. */
static mogbus_server_t* volatile running_server = NULL;

int
main(void)
{
	mogbus_t* ctx = NULL;
	mogbus_server_t* server = NULL;

	/* From now on SIGINT (Ctrl-C) and SIGTERM (what `docker stop` sends) stop the server that
	 * is stored in running_server. Call this once, before creating the server. */
	mogbus_stop_on_signals(&running_server);

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

	if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}
	running_server = server; /* the signals can stop it now */

	/* After a signal, mogbus_server_step() returns MOGBUS_STEP_DONE and the loop ends. */
	while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
	{
	}

	running_server = NULL;
	mogbus_destroy(ctx); /* closes the server and frees everything */
	printf("stopped cleanly\n");
	return 0;
}
```

```python
# stop.py - serve the pump station until Ctrl-C or a SIGTERM, then shut down cleanly.
import signal
import threading

import mogbus

stop = threading.Event()  # set when a signal arrives

# SIGINT (Ctrl-C) and SIGTERM (what `docker stop` sends) set the event instead of killing the script.
signal.signal(signal.SIGINT, lambda signum, frame: stop.set())
signal.signal(signal.SIGTERM, lambda signum, frame: stop.set())

with mogbus.Context() as ctx:
    ctx.load_config("station.scm")
    with ctx.serve(mogbus.ServerOptions(port=1502)).start():
        stop.wait()  # sleeps until a signal sets the event

# Leaving the with blocks closed the server, then the context.
print("stopped cleanly")
```

- **C.** @ref mogbus_stop_on_signals "mogbus_stop_on_signals()" makes SIGINT and SIGTERM stop the server whose address is in the variable you give it. The variable must be a global, and it must stay valid as long as the process runs. @ref mogbus_destroy "mogbus_destroy()" closes the server and frees everything.
- **Python.** The script sets an event when a signal arrives, and leaving the `with` blocks closes the server, then the context. Stop your own threads before the context is closed.

Press Ctrl-C, or run `kill -TERM` on the process: each prints `stopped cleanly`, and the library logs
`stopped`.

## A second thread

So far one thread did everything. A program that models the plant often has a second thread, for
example one that updates measurements while the first serves clients. mogbus locks its context, so
calls like `set_value` are safe from any thread while the server runs, and each call waits at most for the
request being answered at that moment.

This program adds one second to the pump run time (input registers 10 and 11) every second, from its own thread:

```c
/* thread.c - a second thread updates the station while the main thread serves clients. */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include "mogbus.h"

/* Runs on its own thread: adds one second to the "pump run time" (input registers 10 and 11)
 * every second. The library locks the context itself, so calling it from here while the main
 * thread serves is safe. */
static void*
count_seconds(void* argument)
{
	mogbus_device_t* dev = argument;

	for (int seconds = 1; ; seconds++)
	{
		sleep(1);
		mogbus_device_set_value(dev, MOGBUS_INPUT, 10, seconds);
	}
	return NULL;
}

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

	if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	pthread_t counter;

	if (pthread_create(&counter, NULL, count_seconds, mogbus_find_device(ctx, 1)) != 0)
	{
		fprintf(stderr, "error: cannot start the thread\n");
		return 1;
	}

	/* This thread only serves. Press Ctrl-C to stop the whole program. */
	while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
	{
	}

	return 0;
}
```

```python
# thread.py - a second thread updates the station while the server answers clients.
import threading

import mogbus

with mogbus.Context() as ctx:
    ctx.load_config("station.scm")
    dev = ctx.find_device(1)  # the device with unit id 1
    stop = threading.Event()  # set to end the counting thread

    def count_seconds():
        """Runs on its own thread: adds one second to the "pump run time" (input registers 10
        and 11) every second. The library locks the context itself, so this is safe."""
        seconds = 0
        while not stop.wait(1.0):  # waits one second, or returns True as soon as stop is set
            seconds += 1
            dev.set_value("input", 10, seconds)

    counter = threading.Thread(target=count_seconds)
    counter.start()

    with ctx.serve(mogbus.ServerOptions(port=1502)).start():
        with mogbus.Client(port=1502, unit=1) as plc:
            threading.Event().wait(3.5)  # wait a few seconds, as a SCADA screen would
            registers = plc.read_input_registers(10, 2)
            print("pump run time:", mogbus.registers_to_value(registers, "u32"), "s")

    stop.set()    # stop the thread before the context is closed
    counter.join()
```

Build the C program with `-lpthread` if your system needs it. After a few seconds the Python script prints:

```text
pump run time: 3 s
```

and from another terminal, against the C program, `./tools/modbus_probe.py input 10 2 --as u32` prints the
seconds since it started. The rules (what the lock covers, what it does not, and that hooks and log sinks
run with it held) are in [lifetimes and threading in C](../library/library-lifetimes.md#threading) and
[in Python](../python/python-lifetimes.md#threads-and-locks).

## Run it in a container

This part is the Scheme / Bash route: the image holds the `mogbus` program and your config. A container
needs the server to listen on every interface, so make a copy of the config for it:

```bash
sed 's/(port 1502) (seed 7)/(port 1502) (bind "0.0.0.0") (seed 7)/; s/(http (port 9102) (control on))/(http (port 9102) (bind "0.0.0.0"))/' \
  examples/tutorial/pump-station.scm > pump-container.scm
docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
docker run --rm -p 127.0.0.1:1502:1502 -p 127.0.0.1:9102:9102 \
  -v "$PWD/pump-container.scm:/etc/mogbus/mogbus.scm:ro" mogbus:0.0.1
./tools/modbus_probe.py input 0 2 --as float32      # from the host
```

The first `sed` expression opens the Modbus port and the second the HTTP endpoint to the container
network; `-p 127.0.0.1:...` keeps both on loopback of the host. The control API is left off in the
container copy. The mounted file must be readable by UID 10001. The image has a `HEALTHCHECK` that runs `--health`, runs as an
unprivileged user and needs no writable filesystem. See [Docker](../deployment/docker.md), [Docker
Compose](../deployment/docker-compose.md) and [Kubernetes](../deployment/kubernetes.md). A C or Python
program is shipped the way you ship any other program; an image can run it under a supervisor of your choice, with the
stop rules above.

## Where next

- Start from a [profile](../profiles/using-profiles.md) instead of a blank file, or [import a register map](../import/import-overview.md) from a CSV.
- Run several devices behind one port: [unit ids](../modbus/unit-ids.md) and [`any-unit`](../config/device/any-unit.md).
- Use the other transports: [RTU over TCP](../transports/transport-rtu-tcp.md), [UDP](../transports/transport-udp.md), a PTY for a serial client ([RTU](../transports/transport-rtu.md)) and [TLS](../transports/transport-tls.md).
- Add vendor function codes: [user-defined function codes](../modbus/user-defined-function-codes.md).
- Talk to a server from code: the [C client](../library/library-client.md), the [Python client](../python/python-client.md) and [asyncio](../python/python-asyncio.md).
- The full [config reference](../config/format.md), the \ref mogbus.h "C API reference" and the \ref core.py "Python API reference".
