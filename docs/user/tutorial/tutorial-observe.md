# Step 6: Observe it

> **Summary:** See what your client does to the station: the web UI, the health and readiness endpoints, Prometheus metrics and request logs, in the binary, in C and in Python.

The observability endpoint is an HTTP server inside the same process, driven by the same loop as the
Modbus server (no extra thread). In a config it is `(server (http ...))`; in code it is part of the
server options. Keep it on loopback: the control API and the web UI have no authentication.

## The web UI

```bash
./mogbus --web examples/tutorial/pump-station.scm
```

```c
options.http_bind_address = "127.0.0.1";
options.http_port = http_port;
options.http_ui = true;
```

```python
options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1", http_port=free_port(),
                               http_control=True)
```

In Python add `http_ui=True` to the options of step 2 (the example has only `http_control=True`). Open <http://127.0.0.1:9102/> (the config's HTTP port; the C program uses the port of `--http PORT` and Python the one you pass). The page lists the points by table with decoded values refreshed every second
(the refresh rate is a selector, down to 10 ms), lets you write decoded or raw values, set faults, and
plot up to eight points as a strip chart: tick the level and the motor current and watch the pump cycle.
`--web` and `http_ui` turn on the control API of [step 5](tutorial-faults.md), which has no
authentication and logs a WARN. Handy while a Python test runs too. See [Web UI](../observability/web-ui.md).

## Health and metrics

The HTTP endpoint answers without a Modbus client. `/healthz` and `/readyz` are for an orchestrator,
`/metrics` is Prometheus text:

```bash
curl -s localhost:9102/healthz       # ok
curl -s localhost:9102/readyz        # ready
curl -s localhost:9102/metrics | grep -E '^mogbus_(requests|responses)_total'
```

```python
for line in server.metrics().splitlines():
    if line.split(" ")[0] in ('mogbus_requests_total{fc="3"}', 'mogbus_requests_total{fc="5"}'):
        print(line)
```

```text
mogbus_requests_total{fc="2"} 1
mogbus_requests_total{fc="4"} 15
mogbus_responses_total{result="ok"} 16
mogbus_responses_total{result="exception"} 0
```

The same text is available without HTTP: \ref mogbus::core::Server::metrics "Server.metrics()" in Python
(its output on the Python example's run is below) and @ref mogbus_server_metrics "mogbus_server_metrics()"
in C, for a host that exports metrics itself (32 KiB is enough for 16 devices; the C program prints
them before it exits). Counters start at zero when the server is created, and refused requests are
counted like any exception the simulator answers, so a test can assert on them instead of parsing logs
("my logger polled the input registers once a second"):

```text
mogbus_requests_total{fc="3"} 6
mogbus_requests_total{fc="5"} 2
```

The C program's metrics, after the self-test of [step 4](tutorial-guard.md) (one exception `01` for the event log write, two
`03` for the setpoint 9.0 and mode 7 and one `04` for the coil while off):

```text
metrics:
  mogbus_requests_total{fc="3"} 2
  mogbus_requests_total{fc="5"} 2
  mogbus_requests_total{fc="16"} 3
  mogbus_requests_total{fc="20"} 1
  mogbus_requests_total{fc="21"} 1
  mogbus_requests_total{fc="24"} 1
  mogbus_connections_total 3
  mogbus_device_exceptions_total{unit="10",code="1"} 1
  mogbus_device_exceptions_total{unit="10",code="3"} 2
  mogbus_device_exceptions_total{unit="10",code="4"} 1
```

The full list is in [Metrics](../observability/metrics/metrics.md) and the endpoint in [Health, readiness
and metrics](../observability/http-endpoint.md).

For a probe that goes through the Modbus port itself, use `--health` (step 2), which sends one real
request to the first device. The `delay` fault of step 5 is shorter than the 2 s default timeout, so the
probe stays healthy. See [Health check](../deployment/health-check.md).

## Logs

`-v` logs every request, and `--log-format json` writes one JSON object per line for a log pipeline.
In the library, logging is process-wide: by default it writes timestamped `INFO` and `WARN` lines to
stderr, and a *sink* sends each line to a function instead (@ref mogbus_set_log_sink "mogbus_set_log_sink()" in C, \ref mogbus::core::set_log_sink "set_log_sink()"
in Python), synchronously on the thread that logs. The level drops lines below it (`debug` adds one
line per request) and the format switches to JSON, in which case a sink receives the whole object as
its message, which a test can parse.

```bash
./mogbus -v --log-format json examples/tutorial/pump-station.scm
```

```c
static void
log_sink(void* user, mogbus_log_level_t level, const char* message)
{
	(void)user;
	if (level >= MOGBUS_LOG_WARN)
		g_warnings++;
	fprintf(stderr, "  [mogbus %s] %s\n",
		level == MOGBUS_LOG_DEBUG ? "debug" : level == MOGBUS_LOG_INFO ? "info" : "warn", message);
}
...
mogbus_set_log_sink(log_sink, NULL);
```

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
{"ts":"2026-11-27T11:07:58.505Z","level":"debug","msg":"tcp 127.0.0.1:41718 unit=1 fc=0x04 -> ok","event":"request","transport":"tcp","peer":"127.0.0.1:41718","unit":1,"fc":4,"result":"ok","exception":0,"latency_ms":0,"faults":[]}
```

That is a line of the binary. In the Python program the request is parsed back from the sink and
printed as:

```text
a request logged as JSON: {"event": "request", "fc": 3, "result": "ok", "transport": "tcp", "unit": 1}
```

The C program's sink puts the library's lines in its own format and counts warnings (a harness can
fail a test on one); `--verbose` sets `MOGBUS_LOG_DEBUG`:

```text
  [mogbus info] listening on 127.0.0.1:41347 (3 devices). Ctrl-C to stop.
  [mogbus info] stopped
```

The line also carries the peer, a timestamp, the latency and the faults that fired. The Python
`set_log_sink(None)` goes back to stderr. A sink is called on the thread that logs (the server thread
while a server runs), so keep it short and do not block. See [Logs](../observability/logs.md) for the
fields.

## Next

- [Step 7: Test your client](tutorial-test.md)
