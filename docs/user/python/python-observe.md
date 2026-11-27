# Logs, metrics, configs and runtime control

See what your client does to the station, inject faults while it runs, and reuse profiles and other transports.

This page finishes the [tutorial](python-start.md) with steps 7 to 9.

## Step 7: logs, metrics and the control API

**The log.** The library logs every Modbus request at the debug level. Take them with a *sink*: a
callable that receives the level and the message instead of stderr. With the JSON format the message
is one JSON object, which a test can parse:

```python
lines = []
mogbus.set_log_level("debug")
mogbus.set_log_format("json")
mogbus.set_log_sink(lambda level, message: lines.append((level.name, message)))
plc.holding(SETPOINT, 2)
mogbus.set_log_sink(None)
mogbus.set_log_format("text")
mogbus.set_log_level("warn")
request = [json.loads(m) for _, m in lines if '"event":"request"' in m]
keys = ("event", "fc", "result", "transport", "unit")
print("a request logged as JSON:", json.dumps({k: request[-1][k] for k in keys}, sort_keys=True))
```

```text
a request logged as JSON: {"event": "request", "fc": 3, "result": "ok", "transport": "tcp", "unit": 1}
```

The line also carries the peer, a timestamp, the latency and the faults that fired. Logging is
process-wide, see [Threads, locks and shutdown](python-threading.md#logging-is-process-wide).
\ref mogbus::core::set_log_sink "set_log_sink()" with `None` goes back to stderr (the library writes
its start-up lines there; the tests of the example silence them with level `"off"`).

**Metrics.** \ref mogbus::core::Server::metrics "Server.metrics()" returns the Prometheus text of the
server: requests per function code, exceptions, connections, bytes. It is the same text that
`/metrics` serves on the HTTP endpoint the example enabled:

```python
for line in server.metrics().splitlines():
    if line.split(" ")[0] in ('mogbus_requests_total{fc="3"}', 'mogbus_requests_total{fc="5"}'):
        print(line)
```

```text
mogbus_requests_total{fc="3"} 6
mogbus_requests_total{fc="5"} 2
```

A test can assert on those counters ("my logger polled the input registers once a second"). See the
[metrics reference](../observability/metrics/metrics.md) for the names.

**Runtime control.** `http_control=True` in \ref mogbus::core::ServerOptions "ServerOptions" (with
the HTTP endpoint on, which the options set with `http_bind_address` and `http_port`) turns on the
[runtime control API](../observability/api/control-api.md): another process, or the test itself, can
read and write values and inject faults while the server runs. Here the test makes the station
answer exception 06 (busy) to find out what its client does, then clears the fault:

```python
base = f"http://127.0.0.1:{options.http_port}"
urllib.request.urlopen(urllib.request.Request(
    base + "/api/devices/1/faults/exception", data=b'{"exception_code": 6}', method="PUT"), timeout=2)
try:
    plc.holding(SETPOINT, 2)
except ModbusException as exc:
    print("with the fault injected:", exc)
urllib.request.urlopen(urllib.request.Request(
    base + "/api/devices/1/faults/exception", method="DELETE"), timeout=2)
```

```text
with the fault injected: function 0x03 failed with exception 06
```

The Python calls change a device from inside the process; the HTTP API is for other processes and
for faults, which the Python API cannot define (faults and machines come from config files or this
API). The control API has no authentication, so keep it on loopback. `http_ui=True` serves the
[web UI](../observability/web-ui.md) on the same endpoint, handy to watch the station while a test
runs.

## Step 8: a profile from a file

When a device does not need code, describe it as a config and let \ref mogbus::core::serve "serve()"
do the rest: it creates a context, loads the config, serves it from a background thread and yields a
\ref mogbus::core::Served "Served" with the context, the server, the port and `device(unit_id)`. It
cleans up on exit. `config_text` takes the config as a string, `config` a path (CI uses the
[profiles](https://github.com/BolvicBolvicovic/mogbus/tree/main/profiles) of the repository):

```python
with mogbus.serve(config_text='(device (unit-id 1) (holding 0 (static 7)))', port=0) as srv:
    with Client(srv.port) as plc:
        print("config_text:", plc.holding(0))

with mogbus.serve(config=str(ROOT / "profiles" / "flow-meter.scm"), port=0) as srv:
    srv.device(1).set_value("input", 0, 12.5)
    with Client(srv.port) as meter:
        print("flow meter from profiles/flow-meter.scm:", meter.read_f32(4, 0), "m3/h")
```

```text
config_text: [7]
flow meter from profiles/flow-meter.scm: 12.5 m3/h
```

The server uses what the binary would use: the config's own `(server ...)` section, or the defaults
(TCP on 1502) when it has none. Only the keyword arguments you pass override its fields
(`mogbus.serve(config=path, port=0, mode="rtu-tcp", idle_timeout_ms=500)`); `port=0` in a network mode
picks a free loopback port, and without a config you get TCP on a free port. The config is where
machines (a pump model written as expressions) and faults on schedules live, which the Python API
cannot create; the host side (`set_value()`, hooks) still works on the devices it loads, so a
profile and a few lines of Python often replace a whole model class.

## Step 9: the options of a config, other transports

Steps by hand, for when you need the context: \ref mogbus::core::Context::load_config "Context.load_config()"
loads the file, \ref mogbus::core::Context::config_server_options "Context.config_server_options()"
returns its `(server ...)` section as `ServerOptions` (or `None`),
\ref mogbus::core::Context::serve_config "Context.serve_config()" serves exactly that, and
\ref mogbus::core::Context::check_server "Context.check_server()" validates changes first. One
context can run several servers (up to 8) over the same devices, in any transport: TCP, UDP, RTU,
ASCII and their over-TCP and over-UDP variants, and PTY or serial ports for RTU and ASCII.

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
use at a baud rate. A server for a PTY (`mode="rtu-pty"`) exposes `Server.pty_name` for a serial
client to open. A server only serves the devices that exist when it is created, and a context is
limited to 16 devices, 8 servers; machines and faults are written in config files.

## See also

- [Python tutorial](python-start.md)
- [Testing your client](python-testing.md)
- [Health, readiness and metrics endpoint](../observability/http-endpoint.md)
- [Runtime control API](../observability/api/control-api.md)
- \ref core.py "Python API reference"
