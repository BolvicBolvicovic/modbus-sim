# Threads, locks and shutdown

Run the process model on its own thread beside the server, and the rules that make it safe: one lock per context, who runs where, and how to close cleanly.

The C library is not thread-safe, so the package serialises it: every method of a
\ref mogbus::core::Context "Context", \ref mogbus::core::Device "Device" and
\ref mogbus::core::Server "Server" takes one re-entrant lock per context. You do not manage that lock
yourself; you only need to know who holds it when.

## Step 6: the process on a thread

So far the example moved time by calling `tick()`. A live simulation ticks on its own thread while
clients are being served:

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
auto mode, lets the thread run until the pump starts, then switches the mode off over Modbus:

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

## Who runs where

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
  your own threads (the model above) *before* closing the context.
- The context managers do this for you: leaving `with ctx.serve(...).start() as server:` closes the
  server, leaving `with mogbus.Context() as ctx:` closes the context (servers first).
- The package installs no signal handlers. A script that should stop on Ctrl-C catches
  `KeyboardInterrupt` in the main thread and leaves its `with` blocks; a host that serves TLS should
  also ignore `SIGPIPE`.

## Logging is process-wide

\ref mogbus::core::set_log_level "set_log_level()", \ref mogbus::core::set_log_format "set_log_format()"
and \ref mogbus::core::set_log_sink "set_log_sink()" change the log of the whole process, not one
context. A sink is called on the thread that logs (the server thread while a server is started), so
keep it short and do not block; `set_log_sink(None)` restores stderr. See
[Logs, metrics, configs and runtime control](python-observe.md) for an example.

## See also

- [Python tutorial](python-start.md)
- [Driving the process from the host](python-process.md)
- \ref core.py "Python API reference"
