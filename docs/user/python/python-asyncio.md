# Using mogbus in an asyncio program

> **Summary:** Serve a simulated device inside your own event loop with `await server.serve()` (no background thread) and talk to any Modbus server with `AsyncClient`, next to pymodbus-style async code.

The threaded `Server.start()` of [step 2](python-serve.md) does not plug into an event loop. For programs that already run one, mogbus has an asyncio form of both sides: \ref mogbus::core::Server::serve "Server.serve()" steps the server on the loop, and \ref mogbus::core::AsyncClient "AsyncClient" is a \ref mogbus::core::Client "Client" whose calls are coroutines. The threaded mode is unchanged and stays the simplest choice for tests that do not use asyncio.

## Serving in the loop

```python
import asyncio, mogbus

async def main():
    with mogbus.Context() as ctx:
        dev = ctx.add_device(1, "station")
        dev.add_point("holding", 0, "u16", 1234)
        async with ctx.serve(port=1502).running():          # serves as a task of this loop
            async with mogbus.AsyncClient(port=1502) as plc:
                print(await plc.read_holding_registers(0, 1))   # [1234]
            dev.set_value("holding", 0, 99)                 # host calls work as usual

asyncio.run(main())
```

\ref mogbus::core::Server::running "Server.running()" starts \ref mogbus::core::Server::serve "serve()" as a task, and on exit cancels it, runs a final step and closes the server, like `with server:` does for a threaded one. To manage the task yourself:

```python
server = ctx.serve(port=1502)
task = asyncio.create_task(server.serve())    # returns 0 when the server stops, 1 on a poll error
...
server.stop()                                 # from the loop, a hook or another thread: wakes the task
await task
server.close()
```

Everything runs on the loop's thread, with no helper thread:

- Between steps the coroutine waits with the loop's own `add_reader()`/`add_writer()` on the descriptors the library reports (`mogbus_server_poll_fds()`, see [the C side](../library/library-serve.md#driving-the-server-from-your-own-event-loop)): listeners, client connections, UDP or serial descriptors and the HTTP endpoint. An idle server costs no CPU and a request is answered as soon as it arrives. Delayed responses, idle timeouts and RTU timing are honoured through the deadline the library reports.
- Hooks, `set_access_hook()`, `set_written_hook()`, frame and connection hooks, function-code callbacks, run on the loop's thread inside the step, as they run on the server thread in the threaded mode. A slow hook delays the loop and every client, so keep them short. An exception in a hook is printed with its traceback and answered as the hook's documentation says, exactly as in the threaded mode, and serving goes on.
- Host calls such as `set_value()` may be made from coroutines, from other tasks or from other threads; the context lock still serializes them with the steps.
- Where the loop cannot watch descriptors (the Windows `ProactorEventLoop`, or a descriptor that epoll refuses such as a regular file in `rtu-fd`), `serve()` falls back to stepping at most every `poll_interval` seconds (5 ms by default); it is correct but wakes up more often.

A server is driven by one of `start()`, `run()`, `serve()` or your own `step()` calls; asking for a second way raises `MogbusError`.

## Cancellation and errors

Cancelling the task (or leaving `running()`) stops the server, runs a final step so open connections are closed, and then raises `asyncio.CancelledError` as usual. `serve()` does not close the transport; call `Server.close()` (what `running()` does) to free the port. `serve()` also returns when something else ends the server: `stop()`, a finished byte stream or a poll error (result 1).

## Several servers

\ref mogbus::core::Context::serve_all "Context.serve_all()" drives every server of a context with one wait (the Python form of `mogbus_run()`) and returns when all have stopped; with no server it returns at once:

```python
ctx.serve(port=1502)
ctx.serve(mode="udp", port=1502)
task = asyncio.create_task(ctx.serve_all())
```

Servers of different contexts are separate tasks, each with its own `serve()` or `running()`.

## AsyncClient

\ref mogbus::core::AsyncClient "AsyncClient" has the constructor and the methods of `Client`, as coroutines, and works against any server: a mogbus server in the loop or on a thread, the `mogbus` binary or a real device.

```python
async with mogbus.AsyncClient(host="127.0.0.1", port=1502, unit=1) as c:
    regs, coils = await asyncio.gather(c.read_holding_registers(0, 4), c.read_coils(0, 8))
    await c.write_register(10, 500)
```

The C client blocks, so every AsyncClient owns one worker thread and runs each call on it. This has three consequences. The event loop never blocks. The calls of one AsyncClient run one at a time in the order they were made, even when you start them together with `gather()`; use several AsyncClients to talk in parallel. Cancelling an awaiting task abandons the wait but not the request: it completes on the worker thread and later calls queue behind it. `await close()` (or `async with`) waits for the calls in flight, closes the connection and ends the thread. Errors are those of `Client`: `ModbusException` with the exception code, `MogbusError` with a `Status`.

## Example

[examples/python/asyncio_server.py](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/asyncio_server.py) serves a device in the loop next to a ticker task that moves a value, reads it with an `AsyncClient`, installs a hook and serves a TCP and a UDP server with `serve_all()`. `make python-examples` runs it.

## See also

- [Step 2: Serve it and read it](python-serve.md) for the threaded server.
- [Talking to a Modbus server from Python](python-client.md) for the synchronous client.
- [Driving the server from your own event loop](../library/library-serve.md#driving-the-server-from-your-own-event-loop) for the C calls under it.
- \ref core.py "Python API reference"
