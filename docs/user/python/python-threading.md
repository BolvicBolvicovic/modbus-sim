# Threading and logging in Python

The C library is not thread-safe, so the package serialises it: every `Context`, `Device` and `Server` method takes one re-entrant lock per context.

- `server.start()` runs a background thread that calls `step(10 ms)` under that lock and releases it
  between steps. While it runs you may read and write device values from any Python thread; a call
  waits for the current step (at most about 10 ms when the server is idle).
- `server.run()` is the same loop on the calling thread and blocks until `stop()`.
  `run(exclusive=True)` calls `mogbus_server_run()` and holds the lock for the whole run.
- `stop()` takes no lock (it is `mogbus_server_stop()`) and joins the thread. `close()` and
  `Context.close()` stop and join every thread first, then free the C memory; closing twice is safe.
  After `Context.close()` its devices and servers raise `MogbusError("context is closed")`.

## Logging

Logging is process-wide. `set_log_sink(callable)` calls `callable(level, message)` on the thread that
logs (the server thread, with the context lock held), so keep it short and do not block; `None`
restores stderr. `set_log_format("json")` makes `message` a JSON object.

## See also

- [Python API reference](python-api.md)
- [Lifetimes and threading in C](../library/library-lifetimes.md)
