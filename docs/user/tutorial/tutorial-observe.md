# Step 6: Observe it

> **Summary:** See what your client does to the station: the web UI, the health and readiness endpoints, Prometheus metrics and request logs, from the `mogbus` program, from C and from Python.

The observability endpoint is a small HTTP server inside the same process, driven by the same loop as the
Modbus server (no extra thread). In a config it is the `http` entry of the `(server ...)` form; in code
it is part of the server options. The web UI and the control API of [step 5](tutorial-faults.md) have no
password: keep the endpoint on `127.0.0.1`.

## The web UI

The web UI shows every point of the station with its decoded value, lets you write values and set faults,
and draws up to eight points as a strip chart. Start the server with it on, then open
<http://127.0.0.1:9102/> in a browser.

```bash
./mogbus --web pump-station.scm
```

```c
/* observe.c - serve the pump station for 10 seconds with a web UI, a log sink and metrics. */
#include <stdio.h>
#include "mogbus.h"

/* Receives every line the library would log. The library calls it on the thread that steps
 * the server, so keep it short. */
static void
print_log(void* user, mogbus_log_level_t level, const char* message)
{
	static const char* const names[] = { "debug", "info", "warn" }; /* indexed by the level */

	(void)user;
	printf("[%s] %s\n", names[level], message);
}

int
main(void)
{
	mogbus_t* ctx = NULL;
	mogbus_server_t* server = NULL;

	mogbus_set_log_level(MOGBUS_LOG_DEBUG); /* also log every request */
	mogbus_set_log_sink(print_log, NULL);

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

	/* The HTTP endpoint on 127.0.0.1:9102 and the web UI on its front page. The web UI also
	 * switches the control API on; neither has a password, so keep them on 127.0.0.1. */
	options.http_bind_address = "127.0.0.1";
	options.http_port = 9102;
	options.http_ui = true;

	if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	/* Serve for 10 seconds: open http://127.0.0.1:9102/ in a browser meanwhile. */
	uint64_t end_ms = mogbus_now_ms() + 10000;

	while (mogbus_now_ms() < end_ms && mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
	{
	}

	/* The same text as http://127.0.0.1:9102/metrics. */
	char metrics[8192];

	mogbus_server_metrics(server, metrics, sizeof metrics);
	printf("%s", metrics);

	mogbus_destroy(ctx);
	return 0;
}
```

```python
# observe.py - serve the pump station with a web UI, a log sink and metrics.
import mogbus

# Receives every line the library would log. The library calls it from the server's thread,
# so keep it short.
mogbus.set_log_level("debug")  # also log every request
mogbus.set_log_sink(lambda level, message: print(f"[{level.name.lower()}] {message}"))

with mogbus.Context() as ctx:
    ctx.load_config("station.scm")

    # The HTTP endpoint on 127.0.0.1:9102 and the web UI on its front page. The web UI also
    # switches the control API on; neither has a password, so keep them on 127.0.0.1.
    options = mogbus.ServerOptions(port=1502, http_bind_address="127.0.0.1", http_port=9102,
                                   http_ui=True)

    with ctx.serve(options).start() as server:
        with mogbus.Client(port=1502, unit=1) as plc:
            plc.read_holding_registers(0, 2)  # one request, so there is something to count
            plc.read_input_registers(0, 2)

        # The same text as http://127.0.0.1:9102/metrics.
        print(server.metrics())

mogbus.set_log_sink(None)  # back to the default: log lines go to stderr
mogbus.set_log_level("info")
```

- **Scheme / Bash.** `--web` serves the UI on the HTTP port of the config (9102 in `pump-station.scm`; without an `http` entry it uses 9102) and also turns on the control API of step 5.
- **C.** `observe.c` runs for 10 seconds, so open the browser quickly. It sets the HTTP address and port, and `http_ui = true` turns on the UI. A log sink and the metrics are explained below.
- **Python.** The script is over in a moment, because it only makes two requests to count; to keep the UI up, add a `time.sleep(60)` before the `print(server.metrics())` line.

In the UI, tick the tank level and the motor current to see the pump cycle in the chart. See [Web UI](../observability/web-ui.md).

## Health and metrics

The HTTP endpoint answers without a Modbus client. `/healthz` and `/readyz` are for an orchestrator,
`/metrics` is Prometheus text:

```bash
curl -s localhost:9102/healthz       # ok
curl -s localhost:9102/readyz        # ready
curl -s localhost:9102/metrics | grep -E '^mogbus_(requests|responses)_total'
```

```text
ok
ready
mogbus_requests_total{fc="3"} 1
mogbus_requests_total{fc="4"} 1
mogbus_responses_total{result="ok"} 2
mogbus_responses_total{result="exception"} 0
mogbus_responses_total{result="none"} 0
mogbus_responses_total{result="dropped"} 0
```

The last lines show two requests that were both answered. The programs above end by printing the same text without HTTP, with
@ref mogbus_server_metrics "mogbus_server_metrics()" in C and \ref mogbus::core::Server::metrics "Server.metrics()"
in Python, for a program that exports metrics itself. Counters start at zero when the server is created.
A refused request counts like any answered exception, so a test can assert on the counters instead of
reading logs ("my logger polled the input registers once a second"). The full list is in
[Metrics](../observability/metrics/metrics.md) and the endpoint in [Health, readiness and
metrics](../observability/http-endpoint.md).

For a probe that goes through the Modbus port itself, use `mogbus --health` (step 2). See [Health
check](../deployment/health-check.md).

## Logs

`mogbus -v` logs every request, and `--log-format json` writes one JSON object per line for a log
pipeline:

```bash
./mogbus -v --log-format json pump-station.scm
```

```text
{"ts":"2026-11-27T11:07:58.505Z","level":"debug","msg":"tcp 127.0.0.1:41718 unit=1 fc=0x04 -> ok","event":"request","transport":"tcp","peer":"127.0.0.1:41718","unit":1,"fc":4,"result":"ok","exception":0,"latency_ms":0,"faults":[]}
```

In a program, logging is process-wide. By default the library writes `INFO` and `WARN` lines to the error
output. The programs above set a *log sink*, a function that receives each line instead (@ref mogbus_set_log_sink "mogbus_set_log_sink()" in
C, \ref mogbus::core::set_log_sink "set_log_sink()" in Python), and the level `debug`, which adds one line per
request. With the sink the output of the programs starts like this:

```text
[info] HTTP endpoint on 127.0.0.1:9102 (/healthz, /readyz, /metrics)
[warn] HTTP control API enabled on 127.0.0.1:9102: anyone who can reach this port can change device state and faults
[info] listening on 127.0.0.1:1502 (1 device). Ctrl-C to stop.
[debug] client 127.0.0.1:59536 connected
[debug] tcp 127.0.0.1:59536 unit=1 fc=0x03 -> ok
[debug] tcp 127.0.0.1:59536 unit=1 fc=0x04 -> ok
```

To get JSON lines from a program, switch the format with `mogbus_set_log_format(MOGBUS_LOG_JSON)` in C or
`mogbus.set_log_format("json")` in Python: the sink then receives the whole JSON object as its message,
which a test can parse. A sink is called on the thread that logs, so keep it short and do not block.
See [Logs](../observability/logs.md) for the fields.

## Next

- [Step 7: Test your client](tutorial-test.md)
