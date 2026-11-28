# Step 8: Stop it and ship it

> **Summary:** Stop the station cleanly on Ctrl-C or on a shutdown, run the process model on its own thread beside the server, know who holds the lock, and run the config route in a container.

## Stopping

The `mogbus` binary stops on SIGINT and SIGTERM with exit status 0 (which is what `docker stop`
needs). A library program installs no signal handlers unless asked.

```bash
./mogbus examples/tutorial/pump-station.scm      # press Ctrl-C
```

```c
static mogbus_server_t* volatile g_server;
...
mogbus_stop_on_signals(&g_server);
...
g_server = server;
...
done:
	g_server = NULL;
	mogbus_server_close(server);
	mogbus_destroy(ctx);
```

```python
with mogbus.Context() as ctx:
    with ctx.serve(options).start() as server:
        ...                     # leaving the blocks closes the server, then the context
```

- **C.** @ref mogbus_stop_on_signals "mogbus_stop_on_signals()" does what `main.c` does: SIGINT and SIGTERM stop the server stored in a slot you own, and SIGPIPE is ignored. It is process-wide, so call it once, and the slot must stay valid for the life of the process. The handler does nothing while the slot is NULL, so install it before creating the server and store the server afterwards (the last assignment of step 2). @ref mogbus_server_stop "mogbus_server_stop()" is async-signal-safe and may come from another thread: the next `mogbus_server_step()` returns `MOGBUS_STEP_DONE` and the loop of step 2 ends. When it ends, `main()` jumps to its `done:` label (error paths use it too), clears the slot and closes in order: @ref mogbus_server_close "mogbus_server_close()" closes the transport and drops client connections (a file descriptor you passed in stays open), @ref mogbus_destroy "mogbus_destroy()" closes every server still open and releases all the memory; both accept NULL.
- **Python.** The package installs no signal handlers: a script that should stop on Ctrl-C catches `KeyboardInterrupt` in the main thread and leaves its `with` blocks, which close the server and then the context. `Server.stop()` takes no lock and joins the background thread; `Context.close()` stops and joins every thread first and closing twice is safe. After it, devices and servers raise `MogbusError("context is closed")`, so stop your own threads before closing the context.

Pressing Ctrl-C while the C program runs prints:

```text
level 2.25 m, pump ON, events 9, client writes 0 ok / 0 refused
^C
stopping
metrics:
  mogbus_requests_total{fc="8"} 1
  mogbus_connections_total 1
```

## A model on its own thread

So far the C and Python programs advanced simulated time by themselves: C from its loop, Python by
calling `tick()` in the demo. A live simulation ticks on its own thread while clients are being served
(the config route has no such thread; the simulator evaluates machines when requests arrive). The Python
`Process.start()` starts one that calls `tick()` every 50 ms with the real time elapsed times `speed`,
which stretches simulated time (600 means ten simulated minutes per real second) and keeps tests short:

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

The model calls the device from its thread while the server thread answers clients; that is safe
because both go through the context lock. The example puts the station in auto mode, lets the thread
run until the pump starts, then switches the mode off over Modbus:

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

In C the same thing is any thread calling `mogbus_device_*` while another runs
@ref mogbus_server_run "mogbus_server_run()": every call that takes a context, device or server holds one
recursive lock of that context, a step releases it while it waits for I/O and holds it while it
answers requests, so a host call waits at most for the request being served. Hooks and log sinks run on
the thread that steps the server with the lock held, so keep them short and let them wait for nothing.
Stop the server and join the thread that steps it before `mogbus_destroy()`. The full rules (what the
lock covers, what it does not, string and error lifetimes) are in [lifetimes and threading in
C](../library/library-lifetimes.md#threading) and [in Python](../python/python-lifetimes.md#threads-and-locks).

## Run it in a container

This part is the config route: the image holds the `mogbus` binary. A container needs the server to
listen on every interface, so make a copy of the config for it:

```bash
sed 's/(port 1502) (seed 7)/(port 1502) (bind "0.0.0.0") (seed 7)/; s/(http (port 9102))/(http (port 9102) (bind "0.0.0.0"))/' \
  examples/tutorial/pump-station.scm > pump-container.scm
docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
docker run --rm -p 127.0.0.1:1502:1502 -p 127.0.0.1:9102:9102 \
  -v "$PWD/pump-container.scm:/etc/mogbus/mogbus.scm:ro" mogbus:0.0.1
./tools/modbus_probe.py input 0 2 --as float32      # from the host
```

The mounted file must be readable by UID 10001. The image has a `HEALTHCHECK` that runs `--health`, runs
as an unprivileged user and needs no writable filesystem. See [Docker](../deployment/docker.md),
[Docker Compose](../deployment/docker-compose.md) and [Kubernetes](../deployment/kubernetes.md). A
C or Python harness is a program you ship the way you ship any other; an image can run it under a
supervisor of your choice, with the stop rules above.

## Where next

- Start from a [profile](../profiles/using-profiles.md) instead of a blank file, or [import a register map](../import/import-overview.md) from a CSV.
- Run several devices behind one port: [unit ids](../modbus/unit-ids.md) and [`any-unit`](../config/device/any-unit.md).
- Use the other transports: [RTU over TCP](../transports/transport-rtu-tcp.md), [UDP](../transports/transport-udp.md), a PTY for a serial client ([RTU](../transports/transport-rtu.md)) and [TLS](../transports/transport-tls.md).
- Add vendor function codes: [user-defined function codes](../modbus/user-defined-function-codes.md).
- Talk to a server from code: the [C client](../library/library-client.md), the [Python client](../python/python-client.md) and [asyncio](../python/python-asyncio.md).
- The full [config reference](../config/format.md), the \ref mogbus.h "C API reference" and the \ref core.py "Python API reference".
