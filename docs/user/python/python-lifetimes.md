# Step 5: Observe, shut down, lifetimes and threading

> **Summary:** See what your client does to the station through logs, metrics and the runtime control API, run the process model on its own thread beside the server, and know who holds the lock and how to close cleanly.

## Logs

The library logs every Modbus request at the debug level. Take the lines with a *sink*: a callable that
receives the level and the message instead of stderr. With the JSON format the message is one JSON
object, which a test can parse. The example turns the log on, makes one request with its client `plc`,
then restores the defaults and parses the line:

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

The line also carries the peer, a timestamp, the latency and the faults that fired.
\ref mogbus::core::set_log_sink "set_log_sink()" with `None` goes back to stderr (the library writes its
start-up lines there; the tests of the example silence them with level `"off"`).
\ref mogbus::core::set_log_level "set_log_level()", \ref mogbus::core::set_log_format "set_log_format()"
and `set_log_sink()` change the log of the whole process, not one context. A sink is called on the thread
that logs (the server thread while a server is started), so keep it short and do not block.

## Metrics

\ref mogbus::core::Server::metrics "Server.metrics()" returns the Prometheus text of the server:
requests per function code, exceptions, connections, bytes. It is the same text that `/metrics` serves
on the HTTP endpoint the example enabled in step 2. With `server` the started server, the example
prints two counters:

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

## Runtime control

`http_control=True` in \ref mogbus::core::ServerOptions "ServerOptions" (with the HTTP endpoint on,
which the options of step 2 set with `http_bind_address` and `http_port`) turns on the
[runtime control API](../observability/api/control-api.md): another process, or the test itself, can
read and write values and inject faults while the server runs. Here the test makes the station answer
exception 06 (busy) to find out what its client does, then clears the fault (`options` are the server
options of step 2):

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

The Python calls change a device from inside the process; the HTTP API is for other processes and for
faults, which the Python API cannot define (faults and machines come from config files or this API).
The control API has no authentication, so keep it on loopback. `http_ui=True` serves the
[web UI](../observability/web-ui.md) on the same endpoint, handy to watch the station while a test
runs.

## Threads and locks

The C library is thread-safe per context (see the
[C threading rules](../library/library-lifetimes.md#threading)). The package still takes one re-entrant
Python lock per context in every method of a \ref mogbus::core::Context "Context",
\ref mogbus::core::Device "Device" and \ref mogbus::core::Server "Server", because it also guards
the package's own state (closed handles, hook registries, the background thread). You do not manage
that lock yourself; you only need to know who holds it when.

So far the example moved time by calling `tick()` itself. A live simulation ticks on its own thread
while clients are being served. The `start()` method of `Process` starts such a thread, which calls
`tick()` every 50 ms with the real time elapsed times `speed`:

```python
def start(self, speed=1.0, period=0.05):
    def loop():
        last = time.monotonic()
        while not self._stop.wait(period):
            now = time.monotonic()
            self.tick((now - last) * speed)
            last = now
    self._stop.clear()
    self.thread = threading.Thread(target=loop, name="process", daemon=True)
    self.thread.start()
```

`speed` stretches simulated time (600 means ten simulated minutes per real second), which keeps
demonstrations and tests short. The model calls the device from its thread while the server thread
answers clients; that is safe because both go through the context lock. The example puts the station in
auto mode, lets the thread run until the pump starts (`wait_for()` polls a condition for up to three
seconds), then switches the mode off over Modbus:

```python
plc.write_registers(MODE, [AUTO])
process.start(speed=600.0)          # ten simulated minutes per second
wait_for(lambda: plc.discrete(RUNNING)[0] == 1)
print("auto mode: pump running =", bool(plc.discrete(RUNNING)[0]))
plc.write_registers(MODE, [OFF])
wait_for(lambda: plc.discrete(RUNNING)[0] == 0)
process.stop()
print("mode off: pump running =", bool(plc.discrete(RUNNING)[0]))
```

```text
auto mode: pump running = True
mode off: pump running = False
```

Who runs where:

- \ref mogbus::core::Server::start "Server.start()" runs a background thread that steps the server
  every 10 ms under the context lock and releases it between steps. While it runs, any Python
  thread may read and write device values; a call waits for the current step (at most about 10 ms
  when the server is idle).
- \ref mogbus::core::Server::run "Server.run()" is the same loop on the calling thread and blocks
  until `stop()`; `run(exclusive=True)` calls the C `mogbus_server_run()` and holds the lock for the
  whole run, so nothing else can touch the context. \ref mogbus::core::Server::step "Server.step()"
  does one poll iteration for a loop of your own (for example a single-threaded test that interleaves
  client calls and `step()`).
- The **access hook** and the **log sink** run on the thread that steps the server, with the lock held.
  The lock is reentrant, so a hook may call the methods of its device, but both must be short and
  must not wait for another thread that wants the context.
- \ref mogbus::core::probe "probe()" blocks the caller and takes no lock, so it works while a started
  server runs; it cannot probe a server that the same thread steps.
- Your own model thread (as above) is just another caller: it holds the lock for the duration of each
  call, not between them.

## Shutdown

- \ref mogbus::core::Server::stop "Server.stop()" takes no lock (it is the C `mogbus_server_stop()`)
  and joins the background thread. \ref mogbus::core::Server::close "Server.close()" and
  \ref mogbus::core::Context::close "Context.close()" stop and join every thread first, then free
  the C memory; closing twice is safe.
- After `Context.close()` its devices and servers raise `MogbusError("context is closed")`, so stop
  your own threads (the model above) *before* closing the context. The example ends with
  `server.close()` and `ctx.close()` after stopping the process thread.
- The context managers do this for you: leaving `with ctx.serve(...).start() as server:` closes the
  server, leaving `with mogbus.Context() as ctx:` closes the context (servers first).
- The package installs no signal handlers. A script that should stop on Ctrl-C catches
  `KeyboardInterrupt` in the main thread and leaves its `with` blocks; a host that serves TLS should
  also ignore `SIGPIPE`.

## Lifetimes

- The C memory of a context lives until `Context.close()`; there is no per-device removal. The Python
  objects (`Device`, `Server`) are handles that raise `MogbusError` once their context is closed.
- The package keeps the hooks, sinks and function-code callbacks you install alive for as long as they are set, so you do not need to hold a reference yourself. A hook is not part of a config and goes away with its device or server.
- A fallible call raises \ref mogbus::core::MogbusError "MogbusError" carrying the status code and
  the message of the C library (`mogbus_last_error`).

## Next

- [Step 6: Test your client](python-testing.md) turns the station into a fixture.

## See also

- [Step 5 in C](../library/library-lifetimes.md)
- [Metrics reference](../observability/metrics/metrics.md)
- \ref core.py "Python API reference"
