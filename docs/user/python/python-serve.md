# Step 2: Serve it and read it

> **Summary:** Pick a transport, create the server, start it on a background thread and read the station with a client, the way your own code will.

A server exposes every device its context has at the moment it is created, over one transport. Devices added later are not served, so build the devices first, as in [step 1](python-device.md). A context holds up to 8 servers.

## Options

\ref mogbus::core::ServerOptions "ServerOptions" holds the settings of a transport; the defaults are
those of a config file (TCP on `127.0.0.1:1502`, loopback only). The example picks a free port with its
own helper `free_port()` (it binds port 0 and reads back the port the OS chose, so tests can run in
parallel) and turns on the HTTP endpoint that [step 5](python-lifetimes.md) uses.
\ref mogbus::core::Context::check_server "Context.check_server()" validates the options without
starting anything, \ref mogbus::core::Context::serve "Context.serve()" creates the server (`ctx` is
the context of step 1) and \ref mogbus::core::Server::start "Server.start()" runs it on a background
thread, so your program keeps control of the process:

```python
options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1", http_port=free_port(),
                               http_control=True)
ctx.check_server(options)
server = ctx.serve(options).start()
print("alive:", mogbus.probe(options, 1, 1000))
```

\ref mogbus::core::probe "probe()" is the client side of `mogbus --health`: one echo request to unit 1
of the server described by `options`, with a timeout of 1000 ms, true when the server answers. It is how
a test waits for a server to be ready:

```text
alive: True
```

## Transports

One context can run several servers over the same devices, in any transport: TCP, UDP, RTU, ASCII and
their over-TCP and over-UDP variants, and PTY or serial ports for RTU and ASCII (a server for a PTY,
`mode="rtu-pty"`, exposes `Server.pty_name` for a serial client to open). Options can also come from a
config: \ref mogbus::core::Context::config_server_options "Context.config_server_options()" returns the
`(server ...)` section of a file loaded with `Context.load_config()` as `ServerOptions` (or `None`),
\ref mogbus::core::Context::serve_config "Context.serve_config()" serves exactly that, and
`check_server()` validates changes first. The example loads the profile of step 1 into a second context,
checks the same options in four transports (the keyword arguments of `check_server` override fields of
the options), serves one of them and probes it:

```python
with mogbus.Context() as ctx2:
    ctx2.load_config(str(ROOT / "profiles" / "flow-meter.scm"))
    wanted = ctx2.config_server_options()
    print(f"the profile asks for {wanted.mode.name.lower()} on port {wanted.port}")
    for mode in ("tcp", "udp", "rtu-tcp", "ascii-tcp"):
        ctx2.check_server(wanted, mode=mode, port=free_port())
    rtu = mogbus.ServerOptions(mode="rtu-tcp", port=free_port())
    with ctx2.serve(rtu).start():
        print("rtu-tcp server probe:", mogbus.probe(rtu, 1, 1000))
    print("RTU timing at 19200 baud:", mogbus.rtu_timing(19200))
```

```text
the profile asks for tcp on port 1502
rtu-tcp server probe: True
RTU timing at 19200 baud: RtuTiming(t35_us=2006, gap_ms=3)
```

\ref mogbus::core::rtu_timing "rtu_timing()" tells the t3.5 and inter-frame gap a serial server would
use at a baud rate. Machines and faults are written in config files.

## Reading it with a client

The example's client is the small `Client` of `modbus_tcp.py` (see
[the station](python-start.md#the-station)); it stands in for your own client code. Its methods read
and write raw registers (`holding()`, `input()`, `discrete()`), decode a float32 (`read_f32(function
code, address)`, where 3 reads holding and 4 input registers) or a string, and raise `ModbusException`
with the exception code when the server answers an exception. With `options` from above:

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

```text
site: NORTH-01
setpoint: 2.5 mode: 1
level registers: [16384, 0] -> level 2.0
write to the read-only name: function 0x10 failed with exception 02
```

`[16384, 0]` is 2.0 as an IEEE float32 (`0x4000 0x0000`), high word first as the default
`word_order` says. The write to the site name fails with exception 02 because the point is `read_only`
(step 1). The inlet flow ramp is read with the words swapped, as that legacy meter sends them:

```python
first = plc.read_f32(4, INFLOW, low_first=True)
time.sleep(1.2)
second = plc.read_f32(4, INFLOW, low_first=True)
print(f"inlet flow moved by itself: {first:.0f} -> {second:.0f} m3/h")
```

```text
inlet flow moved by itself: 20 -> 24 m3/h
```

The exact numbers depend on when you start; they climb by 4 every second.

In an asyncio program use `await server.serve()` instead of a thread, see [asyncio](python-asyncio.md).

## Next

- [Step 3: Simulate the process from the host](python-simulate.md)

## See also

- [Step 2 in C](../library/library-serve.md)
- [Health, readiness and metrics endpoint](../observability/http-endpoint.md)
- \ref core.py "Python API reference"
