# Step 5: Observe, shut down, lifetimes and threading

Route the library's log lines, read metrics, stop cleanly on Ctrl-C, and know which thread may touch what.

## Logging

Logging is process-wide. By default the library writes timestamped `INFO` and `WARN` lines to stderr. @ref mogbus_set_log_sink "mogbus_set_log_sink()" sends each line to a function instead, synchronously on the thread that logs (truncated to 1023 bytes, valid only during the call); @ref mogbus_set_log_level "mogbus_set_log_level()" drops lines below a level (`DEBUG` adds one line per request) and @ref mogbus_set_log_format "mogbus_set_log_format()" switches to one JSON object per line, in which case a sink receives the whole object as its message.

```c
static void
log_sink(void* user, mogbus_log_level_t level, const char* message)
{
	(void)user;
	if (level >= MOGBUS_LOG_WARN)
		g_warnings++;
	fprintf(stderr, "  [mogbus %s] %s\n", ..., message);
}
...
mogbus_set_log_sink(log_sink, NULL);
```

A harness uses the sink to put the simulator's lines into its own log, or to fail a test on a warning. The program prints the count at the end when it is not zero. `--verbose` sets `MOGBUS_LOG_DEBUG`:

```text
  [mogbus info] listening on 127.0.0.1:41347 (3 devices). Ctrl-C to stop.
  [mogbus info] stopped
```

## Metrics

@ref mogbus_server_metrics "mogbus_server_metrics()" renders the server's counters in the Prometheus text format, the same text the HTTP endpoint serves on `/metrics`, for a host that exports metrics itself. Counters start at zero when the server is created; 32 KiB is enough for 16 devices. The example prints a few lines at the end of the self-test (see the [metrics reference](../observability/metrics/metrics.md)):

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

The vetoes of step 4 are counted like any exception the simulator answers: one `01` (event log write), two `03` (setpoint 9.0 and mode 7) and one `04` (coil while off). A test can assert on them instead of parsing logs.

## Stopping on signals

The library installs no signal handlers unless asked. For a console program, @ref mogbus_stop_on_signals "mogbus_stop_on_signals()" does what `main.c` does: SIGINT and SIGTERM stop the server stored in a slot you own, and SIGPIPE is ignored (a TLS write can raise it when a client vanishes). It is process-wide, so call it once, and the slot must stay valid for the life of the process. The handler does nothing while the slot is NULL, so install it before creating the server and store the server afterwards:

```c
static mogbus_server_t* volatile g_server;
...
mogbus_stop_on_signals(&g_server);
...
g_server = server;
```

@ref mogbus_server_stop "mogbus_server_stop()" is the one call that is async-signal-safe and may come from another thread: the next `mogbus_server_step` returns `MOGBUS_STEP_DONE` and the loop in step 2 ends. Press Ctrl-C while the example runs:

```text
level 2.25 m, pump ON, events 9, client writes 0 ok / 0 refused
^C
stopping
metrics:
  mogbus_requests_total{fc="8"} 1
  mogbus_connections_total 1
```

If you install your own handler instead, have it call `mogbus_server_stop()` on the server (and ignore SIGPIPE in a TLS build).

## Shutting down

Leave the loop, clear the slot, then close in order:

```c
done:
	g_server = NULL;
	mogbus_server_close(server);
	mogbus_destroy(ctx);
```

@ref mogbus_server_close "mogbus_server_close()" closes the transport and releases the handle: client connections are dropped, a file descriptor you passed in stays open, and the slot is reused by the next create. @ref mogbus_destroy "mogbus_destroy()" closes every server still open and releases all the memory, devices included; both accept NULL.

## Lifetimes

- **No `malloc`.** Memory is an `mmap`-backed arena owned by the context. Devices live until `mogbus_destroy()` (there is no per-device removal), so a `mogbus_device_t*` stays valid until then. A `mogbus_server_t*` is valid until `mogbus_server_close()` or `mogbus_destroy()`.
- **Strings.** Strings you pass in (paths, bind address, TLS files, names, string-point text) are copied during the call; free them afterwards. Strings the library returns (@ref mogbus_version "mogbus_version()", @ref mogbus_server_pty_name "mogbus_server_pty_name()", the names in @ref mogbus_device_info "mogbus_device_info()", the options of @ref mogbus_config_server_options "mogbus_config_server_options()") are borrowed and must not be freed or modified.
- **Errors.** The `err` and `err_size` arguments are optional; the message is always NUL-terminated when given.
- **Hooks and sinks.** The `user` pointers you register are yours: they must outlive the hook or sink.

## Threading

- **Not thread-safe.** One thread at a time uses a context. The thread that calls `mogbus_server_step()` is the thread that may touch the devices, including from the access hook, which runs inside the step. A harness that wants a background thread steps the server on it and does all device calls there, or serializes them with its own lock around both the step and the calls.
- The only call that may come from another thread or a signal handler is `mogbus_server_stop()`.
- @ref mogbus_probe "mogbus_probe()" blocks, so call it from another thread or process, as in step 2.
- Logging is process-wide: a sink is called on whichever thread logs, so make it safe for the threads that step servers.

The [Python package](../python/python-threading.md) applies these rules with one lock per context and an optional stepping thread.

## Where next

- [Config reference](../config/format.md) for machines, faults and user-defined function codes the API cannot build, and [profiles](../profiles/using-profiles.md) to load with `mogbus_load_config`.
- [Transports](../transports/transports.md) for the serial, PTY and fd modes, and [TLS](../transports/transport-tls.md) to serve Modbus TCP over TLS from `mogbus_server_options_t`.
- [Observability](../observability/logs.md) for the log fields, `/metrics` and the control API.
- [Python tutorial](../python/python-start.md) to do the same from Python.
