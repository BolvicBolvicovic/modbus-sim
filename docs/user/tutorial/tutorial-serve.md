# Step 2: Serve it and read it

> **Summary:** Start the server on Modbus TCP port 1502, check that it answers and read the station with a client, the way your own code will.

A server shows the devices of its context to Modbus clients over one transport. In this step the device
comes from the `station.scm` file of [step 1](tutorial-device.md). Devices added after the server was
created are not served, so build the devices first.

## Start the server

Pick the address to listen on and the transport (Modbus TCP here). The default, and what the examples
write out, is `127.0.0.1` port 1502: only this computer can connect. Use `0.0.0.0` only when you want
other computers to reach the station.

```bash
./mogbus station.scm
```

```c
/* serve.c - serve the pump station of step 1 over Modbus TCP until the process is killed. */
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

	/* Load the device from the file of step 1 (the same device the code of step 1 builds). */
	if (mogbus_load_config(ctx, "station.scm") != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	/* Where to listen. These are the defaults, written out: Modbus TCP on the port 1502,
	 * reachable from this computer only ("0.0.0.0" would open it to the network). */
	mogbus_server_options_t options = mogbus_server_options_defaults();

	options.mode = MOGBUS_MODE_TCP;
	options.bind_address = "127.0.0.1";
	options.port = 1502;

	if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}
	printf("serving unit 1 on 127.0.0.1:1502, press Ctrl-C to stop\n");

	/* The server only works while you call mogbus_server_step(): it accepts connections, reads
	 * requests and sends answers, waiting at most 100 ms for something to do. It returns
	 * MOGBUS_STEP_CONTINUE as long as the server runs. */
	while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
	{
	}

	mogbus_destroy(ctx);
	return 0;
}
```

```python
# serve.py - serve the pump station of step 1 and read it with a Modbus client.
import mogbus

with mogbus.Context() as ctx:
    # Load the device from the file of step 1 (the same device the code of step 1 builds).
    ctx.load_config("station.scm")

    # Where to listen: Modbus TCP on port 1502, reachable from this computer only
    # ("0.0.0.0" would open it to the network).
    options = mogbus.ServerOptions(mode="tcp", bind_address="127.0.0.1", port=1502)

    # start() serves on a background thread, so the script can go on and be the client.
    with ctx.serve(options).start():
        # A client for 127.0.0.1:1502; every call below talks to unit 1.
        with mogbus.Client(host="127.0.0.1", port=1502, unit=1) as plc:
            # Holding registers 0 and 1: the setpoint, a float32 in two registers.
            registers = plc.read_holding_registers(0, 2)
            setpoint = mogbus.registers_to_value(registers, "f32")
            print(f"setpoint: registers {registers} = {setpoint} m")

            # Holding registers 10 to 17: the site name, 8 registers of two characters.
            name_registers = plc.read_holding_registers(10, 8)
            print("site name:", mogbus.registers_to_text(name_registers))

            # Writing the read-only site name is refused with Modbus exception 02.
            try:
                plc.write_register(10, 0x4F4B)
            except mogbus.ModbusException as exc:
                print("write to the site name refused: exception", exc.code)

        print("health check:", mogbus.probe(options, 1, 1000))  # True while the server answers
```

- **Scheme / Bash.** The `mogbus` program reads the `(server ...)` form of the file and serves until you press Ctrl-C. Add `--check` to validate the file without opening a port.
- **C.** @ref mogbus_server_options_defaults "mogbus_server_options_defaults()" returns the same defaults as a config file. @ref mogbus_server_create "mogbus_server_create()" opens the port: it fails with `MOGBUS_ERR_LISTEN` when the port is in use. Your program runs the server itself with @ref mogbus_server_step "mogbus_server_step()"; the steps after this one put their own work in that loop. Ctrl-C ends the program at once: [step 8](tutorial-ship.md) shows a clean shutdown.
- **Python.** \ref mogbus::core::Context::serve "Context.serve()" makes the server and \ref mogbus::core::Server::start "Server.start()" runs it on a background thread, so the script continues and can be a client of its own server.

The same devices can be served over any other transport (Modbus UDP, RTU or ASCII over TCP, a serial line
or a PTY) by changing only the `mode`; the devices do not change. For example, RTU frames over TCP:

```scheme
(server (mode rtu-tcp) (port 1502))
```

```c
options.mode = MOGBUS_MODE_RTU_TCP; /* in serve.c above, before mogbus_server_create() */
```

```python
options = mogbus.ServerOptions(mode="rtu-tcp", port=1502)  # instead of the options of serve.py above
```

[Transports](../transports/transports.md) lists the modes and what each one needs.

## Read it with a client

Any Modbus client can talk to `127.0.0.1:1502`, unit 1. The Python script above already is one: it reads the
setpoint (holding 0-1, a float32 in two registers) and the site name (holding 10-17), and tries to write
the read-only name. For the Scheme and C routes, a server runs in one terminal and a client in a second. For
the client, use the tiny Python tool `tools/modbus_probe.py` (no dependencies):

```bash
./tools/modbus_probe.py holding 0 2 --as float32     # setpoint
./tools/modbus_probe.py holding 10 8 --as string     # site name
./tools/modbus_probe.py discrete 0 3                 # high alarm, low alarm, pump running
./tools/modbus_probe.py input 0 2 --as float32       # tank level
./tools/modbus_probe.py write-reg 10 5               # write the read-only site name
```

```text
[2.5]
['NORTH-01']
[0, 0, 0]
[2.5]
error: exception 0x02: illegal data address
```

or, for the C route, a client program of your own. The C library has a client too:

```c
/* read.c - a Modbus client: read the pump station's setpoint and site name from the server. */
#include <stdio.h>
#include "mogbus.h"

int
main(void)
{
	mogbus_client_t* plc = NULL;

	/* The client defaults are TCP to 127.0.0.1:1502, the address of the server of this step. */
	mogbus_client_options_t options = mogbus_client_options_defaults();

	if (mogbus_client_open(&options, &plc) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(NULL));
		return 1;
	}

	/* Holding registers 0 and 1 of unit 1: the setpoint, a float32 in two registers. */
	uint16_t registers[2] = { 0, 0 };
	double setpoint = 0;

	if (mogbus_client_read_holding_registers(plc, 1, 0, 2, registers) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_client_last_error(plc));
		return 1;
	}
	mogbus_registers_to_value(registers, MOGBUS_TYPE_F32, MOGBUS_WORDS_HIGH_FIRST,
				  MOGBUS_BYTES_BIG, &setpoint);
	printf("setpoint: registers %u %u = %.1f m\n", (unsigned)registers[0], (unsigned)registers[1],
	       setpoint);

	/* Holding registers 10 to 17 of unit 1: the site name, 8 registers of two characters. */
	uint16_t name_registers[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	char site[17] = "";

	if (mogbus_client_read_holding_registers(plc, 1, 10, 8, name_registers) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_client_last_error(plc));
		return 1;
	}
	mogbus_registers_to_text(name_registers, 8, MOGBUS_BYTES_BIG, site, sizeof site);
	printf("site name: %s\n", site);

	/* Writing the read-only site name is refused with Modbus exception 02. */
	if (mogbus_client_write_register(plc, 1, 10, 0x4F4B) != MOGBUS_OK)
		printf("write to the site name refused: exception %u\n",
		       (unsigned)mogbus_client_last_exception(plc));

	mogbus_client_close(plc);
	return 0;
}
```

Build it like the server, start `./serve` in one terminal and run `./read` in another. It prints:

```text
setpoint: registers 16416 0 = 2.5 m
site name: NORTH-01
write to the site name refused: exception 2
```

The Python script prints the same three lines (its register list is written `[16416, 0]`), then the
health check:

```text
setpoint: registers [16416, 0] = 2.5 m
site name: NORTH-01
write to the site name refused: exception 2
health check: True
```

`16416 0` are the two registers `0x4020 0x0000` of the float32 2.5, high word first. The write to the site
name is refused with exception 02, as for an address with no point at all: see [client
writes](../config/behaviours/client-writes.md) and [Modbus exceptions](../modbus/exceptions.md). The client
calls are those of the [C client](../library/library-client.md) and the [Python
client](../python/python-client.md).

## Check that the server answers

`mogbus --health station.scm` sends one real Modbus request (function code 08, echo) to the first device
and exits with status 0 when it answers. It works against the Scheme / Bash server and against the C and
Python programs, since all of them serve port 1502:

```bash
./mogbus --health station.scm; echo $?       # 0 while a server is running
```

The Python script above ends with the same check as a function, \ref mogbus::core::probe "probe()". In C,
@ref mogbus_probe "mogbus_probe()" is the same check, but it waits for the answer, so the thread that
steps the server must not call it: use `mogbus --health`, or call it from another process or thread. See
[Health check](../deployment/health-check.md).

## Next

- [Step 3: Make it behave](tutorial-simulate.md)
