# Step 2: Serve it and read it

> **Summary:** Pick a transport, start the server, check that it answers and read the station with a client, the way your own code will.

A server exposes every device its context has at the moment it is created, over one transport. Devices
added later are not served, so build the devices first, as in [step 1](tutorial-device.md). One
context holds up to 8 servers, each over its own transport, all over the same devices.

## Start the server

With the config route, the `mogbus` binary is the server: it reads the `(server ...)` form and runs
until Ctrl-C. In C you set the options of a @ref mogbus_server_options_t "mogbus_server_options_t" and
create the server yourself; in Python you fill \ref mogbus::core::ServerOptions "ServerOptions". In
both, the defaults are those of a config file (TCP on `127.0.0.1:1502`, loopback only; `0.0.0.0` is an
explicit opt-in for remote access), and a check validates the options without opening anything, so a
port that is in use still passes.

```bash
./mogbus --check=verbose station.scm
./mogbus station.scm
```

```c
mogbus_server_options_t options = mogbus_server_options_defaults();
...
options.mode = MOGBUS_MODE_TCP;
options.bind_address = "127.0.0.1";
options.port = free_port();
...
if (mogbus_server_check(ctx, &options) != MOGBUS_OK) { ... }
if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
{
	fprintf(stderr, "server: %s\n", mogbus_last_error(ctx));
	goto done;
}
g_server = server;
```

```python
options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1", http_port=free_port(),
                               http_control=True)
ctx.check_server(options)
server = ctx.serve(options).start()
print("alive:", mogbus.probe(options, 1, 1000))
```

- `free_port()` is a helper of both programs: it binds port 0 and reads back the port the OS chose, so tests can run in parallel.
- The Python options also turn on the HTTP endpoint with the control API, which [step 5](tutorial-faults.md) uses.
- @ref mogbus_server_create "mogbus_server_create()" hands the server back through its last parameter (NULL on failure) and returns a @ref mogbus_result_t "mogbus_result_t" that tells why: `MOGBUS_ERR_INVALID` (bad options, no devices, a limit), `MOGBUS_ERR_SERIAL` (serial device or PTY) or `MOGBUS_ERR_LISTEN` (port in use or TLS setup). `mogbus` maps serial to exit status 1 and listen to 2. `g_server` is a global that [step 8](tutorial-ship.md) explains.
- \ref mogbus::core::Server::start "Server.start()" runs the server on a background thread, so your program keeps control of the process.

## Transports

The same devices can be served over any transport: Modbus TCP (optionally with TLS), UDP, RTU, ASCII,
RTU and ASCII over TCP or UDP, and PTY or serial ports for RTU and ASCII. In a config it is the `mode`
of the server, in the options it is the `mode` field (a `MOGBUS_MODE_*` value in C such as
`MOGBUS_MODE_RTU_TCP`, a string in Python). In C, which fields a mode reads is listed on @ref mogbus_server_options_t "the options
struct"; fields a mode does not use are ignored.

```scheme
(server (mode rtu-tcp) (port 1502))
```

```python
with mogbus.Context() as ctx2:
    ctx2.load_config(str(ROOT / "profiles" / "flow-meter.scm"))
    wanted = ctx2.config_server_options()
    print(f"the profile asks for {wanted.mode.name.lower()} on port {wanted.port}")
    for mode in ("tcp", "udp", "rtu-tcp", "ascii-tcp"):
        ctx2.check_server(wanted, mode=mode, port=free_port())
    rtu = mogbus.ServerOptions(mode="rtu-tcp", port=free_port())
    with ctx2.serve(rtu).start():
        alive = mogbus.probe(rtu, 1, 1000)
        print("rtu-tcp server probe:", alive)
    print("RTU timing at 19200 baud:", mogbus.rtu_timing(19200))
```

```text
the profile asks for tcp on port 1502
rtu-tcp server probe: True
RTU timing at 19200 baud: RtuTiming(t35_us=2006, gap_ms=3)
```

The Python example loads a profile into a second context, checks the same options in four transports
(the keyword arguments of `check_server` override fields), serves one of them and probes it;
\ref mogbus::core::rtu_timing "rtu_timing()" tells the t3.5 and inter-frame gap a serial server would use at
a baud rate. See [Transports](../transports/transports.md) for the modes, including
[RTU over TCP](../transports/transport-rtu-tcp.md), [UDP](../transports/transport-udp.md), a PTY for a serial
client ([RTU](../transports/transport-rtu.md)) and [TLS](../transports/transport-tls.md), and the C
[options and several servers](../library/library-serve.md).

## Who steps the server

The `mogbus` binary runs the server loop for you. In Python `Server.start()` steps it on a background
thread (and `await server.serve()` fits an [asyncio program](../python/python-asyncio.md)). In C you own the
thread: @ref mogbus_server_step "mogbus_server_step()" runs one poll iteration (accept, read, answer,
write whatever is ready) and returns after at most `max_wait_ms`. Interleave it with your own work; the
wait doubles as your loop's timer, so 100 ms here means the simulation of [step 3](tutorial-simulate.md)
advances at least ten times a second even with no clients. This part is C only:

```c
while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
{
	double t = now_s();

	simulate(&st, (t - last) * speed);
	last = t;
	...
}
```

It returns `MOGBUS_STEP_CONTINUE`, `MOGBUS_STEP_DONE` (stopped, or the byte-stream peer went away) or
`MOGBUS_STEP_ERROR`. If you have nothing else to do, @ref mogbus_server_run "mogbus_server_run()" steps
until stopped. Requests are answered inside the step, so the simulation, the hooks of step 4 and a host
write never run concurrently with a request; a long computation between steps still delays every client.
Hosts with an event loop of their own, see [serving from C](../library/library-serve.md#driving-the-server-from-your-own-event-loop).

## Check that it answers

`--health` is a client of the server's Modbus port: one real request (FC 08 echo) to the first device,
exit status 0 when it answers. @ref mogbus_probe "mogbus_probe()" and \ref mogbus::core::probe "probe()" are the same
check as a function, which is how a test waits for a server to be ready; it works for the network
modes (a wildcard bind is reached through loopback, a TLS server is only connected to).

```bash
./mogbus --health station.scm; echo $?       # 0 while the server runs
```

```c
pid_t pid = fork();
if (pid == 0)
{
	bool ok = mogbus_probe(options, unit, 2000) == MOGBUS_OK;
	...
	_exit(ok ? 0 : 1);
}
for (int i = 0; i < 1000; i++)
{
	if (waitpid(pid, &status, WNOHANG) == pid)
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	mogbus_server_step(server, 5);
}
```

```python
print("alive:", mogbus.probe(options, 1, 1000))
```

```text
alive: True
```

(The text block is the Python output; the C program prints `ok   mogbus_probe answers from a child
process`.) `mogbus_probe()` blocks, so the thread that steps the C server cannot call it: the program's
`probe_self()` forks a child that probes while the parent keeps stepping until the child exits. A
real program uses a probe thread or an external `mogbus --health`. See [Health check](../deployment/health-check.md).

## Read it with a client

Any Modbus client works against `127.0.0.1:1502`, unit 1. The Config and C routes use
[`tools/modbus_probe.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/tools/modbus_probe.py),
a Modbus TCP client with no dependencies, from a second terminal; the Python program uses its own small
`Client` (class of `modbus_tcp.py`), whose methods read and write raw registers (`holding()`,
`input()`, `discrete()`), decode a float32 (`read_f32(function code, address)`, where 3 reads holding
and 4 input registers) or a string, and raise `ModbusException` with the exception code when the
server answers an exception.

```bash
./tools/modbus_probe.py holding 0 2 --as float32     # setpoint
./tools/modbus_probe.py holding 10 8 --as string     # site name
./tools/modbus_probe.py discrete 0 3                 # high, low, running
./tools/modbus_probe.py input 0 2 --as float32       # level
./tools/modbus_probe.py write-reg 10 5               # read-only site name
```

```c
for (int i = 0; i < 2000; i++)
{
	if (mogbus_server_step(server, 1) != MOGBUS_STEP_CONTINUE) return -1;
	if (poll(&pfd, 1, 0) > 0) { n = read(fd, reply + got, 260 - got); ... }
	if (got >= 7 && got >= 6 + (size_t)(reply[4] << 8 | reply[5])) return (int)got - 7;
}
```

```python
with Client(options.port) as plc:
    print("site:", plc.read_string(SITE_NAME, 8))
    print("setpoint:", plc.read_f32(3, SETPOINT), "mode:", plc.holding(MODE)[0])
    print("level registers:", plc.input(LEVEL, 2), "-> level", plc.read_f32(4, LEVEL))
    try:
        plc.write_registers(SITE_NAME, [0x4F4B])
    except ModbusException as exc:
        print("write to the read-only name:", exc)
```

The C route reads with `--self-test`, a mode of the tutorial program (not of mogbus) in which the
program is its own Modbus client: its helper `exchange()` writes one request on a socket, then steps
the server between reads until the reply is complete, so it needs no second thread or process. The
same trick tests any harness that needs a client and a server in one process. Each check prints an `ok`
or `FAIL` line and the program exits 0 when all pass:

```text
  ok   FC 03 reads the setpoint, 2.5 m
  ok   FC 03 reads the site name string
```

The `modbus_probe.py` commands print (the last one is refused: the site name is read-only, and an
address with no point at all answers the same):

```text
[2.5]
['NORTH-01']
[0, 0, 0]
[2.5]
error: exception 0x02: illegal data address
```

and the Python program:

```text
site: NORTH-01
setpoint: 2.5 mode: 1
level registers: [16384, 0] -> level 2.0
write to the read-only name: function 0x10 failed with exception 02
```

`[16384, 0]` is 2.0 as an IEEE float32 (`0x4000 0x0000`), high word first as the default word order
says. The inlet flow ramp of step 1 is read with the words swapped, as that legacy meter sends them,
and moves by itself:

```python
first = plc.read_f32(4, INFLOW, low_first=True)
time.sleep(1.2)
second = plc.read_f32(4, INFLOW, low_first=True)
print(f"inlet flow moved by itself: {first:.0f} -> {second:.0f} m3/h")
```

```text
inlet flow moved by itself: 20 -> 24 m3/h
```

The exact numbers depend on when you start; they climb by 4 every second. The rules for refused
writes are in [client writes](../config/behaviours/client-writes.md) and [Modbus
exceptions](../modbus/exceptions.md).

## Next

- [Step 3: Make it behave](tutorial-simulate.md)
