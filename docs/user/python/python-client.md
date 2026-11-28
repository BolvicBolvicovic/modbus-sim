# Talking to a Modbus server from Python

Use \ref mogbus::core::Client "Client" to read and write a Modbus server from a script or a test, without a third-party Modbus library.

A Client is a Modbus master. It is independent of every \ref mogbus::core::Context "Context" and \ref mogbus::core::Server "Server": it writes to an endpoint over the wire, so it can query a server that runs in the same process on its background thread, the `mogbus` binary, or a real device. It wraps the [C client](../library/library-client.md).

## Connect and call

```python
import mogbus

with mogbus.Client(mode="tcp", host="127.0.0.1", port=1502, unit=1, timeout_ms=1000) as c:
    print(c.read_holding_registers(0, 4))        # [100, 101, 102, 103]
    c.write_register(10, 500)
    c.write_registers(20, [0x41C8, 0x0000])
    c.mask_write_register(30, 0x00FF, 0x0F00)
    print(c.read_coils(0, 8))                    # [False, True, ...]
    print(c.request(0x41, b"\x01"))              # any function code: the reply data as bytes
```

`mode` is `"tcp"`, `"udp"`, `"rtu-tcp"`, `"rtu-udp"`, `"ascii-tcp"`, `"ascii-udp"`, or `"rtu-device"` and `"ascii-device"` with `path`, `baud`, `data_bits`, `parity` and `stop_bits` for a serial line. `unit` is the default unit id; every call takes `unit=` to ask another one. The client connects on the first call (or on \ref mogbus::core::Client::connect "connect()") and again on the first call after a connection error. `retries` repeats a call that timed out.

| Method | Function code | Returns |
|---|---|---|
| `read_coils(address, count)` | 01 | list of bool |
| `read_discrete_inputs(address, count)` | 02 | list of bool |
| `read_holding_registers(address, count)` | 03 | list of int |
| `read_input_registers(address, count)` | 04 | list of int |
| `write_coil(address, value)` | 05 | |
| `write_register(address, value)` | 06 | |
| `write_coils(address, values)` | 15 | |
| `write_registers(address, values)` | 16 | |
| `mask_write_register(address, and_mask, or_mask)` | 22 | |
| `read_write_registers(read_address, read_count, write_address, values)` | 23 | list of int |
| `read_device_id()` | 43 (MEI 14) | dict of object id to bytes |
| `request(function_code, data=b"")` | any | the reply data as bytes |

Registers are raw 16-bit values; decode floats and strings yourself, for example with `struct.pack(">HH", *regs)`.

## Errors

A Modbus exception raises \ref mogbus::core::ModbusException "ModbusException", a subclass of \ref mogbus::core::MogbusError "MogbusError", with the exception code in `.code`. Everything else is a `MogbusError` whose `status` says what happened: `Status.TIMEOUT` (no reply, after the retries), `Status.IO` (connect, send or receive failed, or the server closed the connection), `Status.FRAME` (a malformed reply) or `Status.INVALID` (a bad argument, nothing sent).

```python
try:
    c.read_holding_registers(5000, 1)
except mogbus.ModbusException as e:
    print("exception", e.code)                   # 2: illegal data address
except mogbus.MogbusError as e:
    print(e.status, e)                           # Status.TIMEOUT, ...
```

## Against a server of the same process

```python
with mogbus.Context() as ctx:
    ctx.add_device(1).add_point("holding", 0, "u16", 1234)
    with ctx.serve(port=1502).start():           # steps on a background thread
        with mogbus.Client(port=1502) as c:
            assert c.read_holding_registers(0, 1) == [1234]
```

The server must be started (`start()`): the client blocks until the reply arrives, so it cannot share the thread that steps the server. A Client takes its own lock per call, so threads may share one, though it handles one request at a time; use one Client per thread to talk in parallel.

## From asyncio

\ref mogbus::core::AsyncClient "AsyncClient" has the same constructor and methods as coroutines (`await c.read_holding_registers(0, 4)`) and never blocks the event loop; see [asyncio](python-asyncio.md).

## Where next

- [Talking to a Modbus server from C](../library/library-client.md) for the C API under it.
- [Using mogbus in an asyncio program](python-asyncio.md) to serve in the event loop and use `AsyncClient`.
- [Testing with pytest or unittest](python-testing.md) to use a Client and a server in one test.
- [User-defined function codes](../modbus/user-defined-function-codes.md#calling-a-user-defined-code-with-the-mogbus-client) for `request()` with a vendor code.
