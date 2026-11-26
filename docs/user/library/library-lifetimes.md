# Lifetimes and threading

What the library allocates, who owns what, which thread may call what, and how signals are handled.

- **Not thread-safe.** One thread at a time uses a context. The thread that calls
  `mogbus_server_step()` is the one that may touch the devices. Only `mogbus_server_stop()` may be
  called from another thread or a signal handler.
- **No `malloc`.** Memory is a `mmap`-backed arena owned by the context. Devices live until
  `mogbus_destroy()`; servers live until `mogbus_server_close()` (destroy closes the rest), and a
  closed server's slot is reused by the next create.
- **Strings.** Strings you pass in are copied; strings the library returns are borrowed. Errors are
  written to the optional `err` and `err_size` buffer.
- **Logging is process-wide.** `mogbus_set_log_sink()` receives each line synchronously on the logging
  thread; the default writes `INFO` and `WARN` lines to stderr. `mogbus_set_log_format()` selects
  text or JSON; a sink then receives the whole JSON object as its message.

## Signals

The library installs no signal handlers unless asked. TLS writes can raise SIGPIPE: ignore it in the
host. `mogbus_server_stop()` is signal-safe, so a host handler can call it.

For a console program, `mogbus_stop_on_signals(&slot)` does it for you, as `main.c` does: SIGINT and
SIGTERM stop the server stored in `slot` (a `mogbus_server_t* volatile` of the host; nothing happens
while it is NULL), and SIGPIPE is ignored. It is process-wide, so call it once.

## See also

- [C API overview](library-api.md)
- [Threading in the Python package](../python/python-threading.md)
- [Logs](../observability/logs.md)
