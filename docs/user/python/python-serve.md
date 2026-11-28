# Serving from Python

Pick a transport, create the server, start it on a background thread and read the station with a client, the way your own code will.

A server exposes every device its context has at the moment it is created, over one transport. Devices added later are not served, so build the devices first, as in [step 1](python-device.md). A context holds up to 8 servers.

## Options

\ref mogbus::core::ServerOptions "ServerOptions" holds the settings of a transport; the defaults are
those of a config file (TCP on `127.0.0.1:1502`, loopback only). The example picks a free port with its
own helper `free_port()` (it binds port 0 and reads back the port the OS chose, so tests can run in
parallel) and turns on the HTTP endpoint that [runtime control](python-lifetimes.md#runtime-control) uses.
\ref mogbus::core::Context::check_server "Context.check_server()" validates the options without
starting anything, \ref mogbus::core::Context::serve "Context.serve()" creates the server (`ctx` is
the context) and \ref mogbus::core::Server::start "Server.start()" runs it on a background
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
`check_server()` validates changes first. The example loads a profile into a second context,
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

## See also

- [Serving from C](../library/library-serve.md)
- [Tutorial step 2: Serve it and read it](../tutorial/tutorial-serve.md)
- [Health, readiness and metrics endpoint](../observability/http-endpoint.md)
- \ref core.py "Python API reference"
