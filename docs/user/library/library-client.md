# Talking to a Modbus server from C

Use the mogbus client to read and write a Modbus server (a real device, the `mogbus` binary or a server of your own process) with one call per function code.

The server side of the library simulates devices. The client side does the opposite: it is a Modbus master that sends requests to an endpoint and returns the replies. A @ref mogbus_client_t "mogbus_client_t" is independent of every @ref mogbus_t "mogbus_t" and of every server: it writes to an endpoint over the wire (or a serial line) and shares no memory with the server, even when the server is in the same process on another thread.

## Open a client

Start from @ref mogbus_client_options_defaults "mogbus_client_options_defaults()" (TCP to `127.0.0.1:1502`, 1000 ms timeout, no retries), change what you need and open the client:

```c
mogbus_client_options_t o = mogbus_client_options_defaults();
o.mode       = MOGBUS_MODE_RTU_TCP;   /* or TCP, UDP, RTU_UDP, ASCII_TCP, ASCII_UDP */
o.host       = "192.168.1.20";        /* an IP address, or "localhost" */
o.port       = 502;
o.timeout_ms = 500;
o.retries    = 2;                     /* extra attempts after a timeout */

mogbus_client_t* c = mogbus_client_open(&o);
if (!c)
	fprintf(stderr, "client: %s\n", mogbus_last_error(NULL));
```

| Mode | Fields |
|---|---|
| `MOGBUS_MODE_TCP`, `UDP`, `RTU_TCP`, `RTU_UDP`, `ASCII_TCP`, `ASCII_UDP` | `host`, `port` |
| `MOGBUS_MODE_RTU_DEVICE`, `ASCII_DEVICE` (a serial line) | `path`, `baud`, `data_bits`, `parity`, `stop_bits` |

The other modes are for servers and are refused. TLS is not supported by the client yet: `tls_enabled` makes @ref mogbus_client_open "mogbus_client_open()" fail.

@ref mogbus_client_open "mogbus_client_open()" only checks the options and allocates; the connection is made by the first call. Call @ref mogbus_client_connect "mogbus_client_connect()" to connect at once and learn early that the endpoint is not there.

Because there is no client yet to own the message of a failed open, it is kept per thread, like the message of @ref mogbus_probe "mogbus_probe()": read it with `mogbus_last_error(NULL)`. Every later call reports its error through its return value, and the client keeps the text: @ref mogbus_client_last_error "mogbus_client_last_error()".

## Read and write

Every call takes the unit id of the server to ask and returns a @ref mogbus_error_t "mogbus_error_t". Reads fill a buffer you pass in: bits as one byte each (0 or 1), registers as raw 16-bit values.

```c
uint16_t regs[4];
uint8_t  bits[8];

if (mogbus_client_read_holding_registers(c, 1, 0, 4, regs) != MOGBUS_OK)
	fprintf(stderr, "read: %s\n", mogbus_client_last_error(c));

mogbus_client_read_coils(c, 1, 0, 8, bits);
mogbus_client_write_register(c, 1, 10, 500);
mogbus_client_write_registers(c, 1, 20, 2, (uint16_t[]){ 0x41C8, 0x0000 });
mogbus_client_mask_write_register(c, 1, 30, 0x00FF, 0x0F00);
```

| Call | Function code |
|---|---|
| @ref mogbus_client_read_coils "mogbus_client_read_coils()" | 01 |
| @ref mogbus_client_read_discrete_inputs "mogbus_client_read_discrete_inputs()" | 02 |
| @ref mogbus_client_read_holding_registers "mogbus_client_read_holding_registers()" | 03 |
| @ref mogbus_client_read_input_registers "mogbus_client_read_input_registers()" | 04 |
| @ref mogbus_client_write_coil "mogbus_client_write_coil()" | 05 |
| @ref mogbus_client_write_register "mogbus_client_write_register()" | 06 |
| @ref mogbus_client_write_coils "mogbus_client_write_coils()" | 15 |
| @ref mogbus_client_write_registers "mogbus_client_write_registers()" | 16 |
| @ref mogbus_client_mask_write_register "mogbus_client_mask_write_register()" | 22 |
| @ref mogbus_client_read_write_registers "mogbus_client_read_write_registers()" | 23 |
| @ref mogbus_client_read_device_id "mogbus_client_read_device_id()" | 43 (MEI 14, basic objects) |
| @ref mogbus_client_request "mogbus_client_request()" | any, including user-defined codes |

The quantities are the protocol's limits (2000 bits or 125 registers per read, 1968 coils or 123 registers per write); a call over the limit fails with `MOGBUS_ERR_INVALID` before anything is sent. The client checks the reply: a write must echo its request, a read must return the number of values asked for, otherwise the call fails with `MOGBUS_ERR_FRAME`.

## Errors

| Result | Meaning |
|---|---|
| `MOGBUS_ERR_EXCEPTION` | the server answered with a Modbus exception; the code is @ref mogbus_client_last_exception "mogbus_client_last_exception()" |
| `MOGBUS_ERR_TIMEOUT` | no reply within `timeout_ms`, after `retries` further attempts |
| `MOGBUS_ERR_IO` | connecting, sending or receiving failed, or the server closed the connection |
| `MOGBUS_ERR_FRAME` | a malformed reply, or one that does not answer the request |
| `MOGBUS_ERR_INVALID` | a bad argument; nothing was sent |

```c
if (mogbus_client_read_holding_registers(c, 1, 5000, 1, regs) == MOGBUS_ERR_EXCEPTION)
	printf("exception %u\n", mogbus_client_last_exception(c));   /* 2: illegal data address */
```

A timeout is the only error that is retried. A request that timed out may still have reached the server, so a retried write can be applied twice: keep `retries` at 0 for writes that must not repeat. A Modbus exception is an answer and leaves the connection usable.

## Connections

A stream connection (TCP, RTU or ASCII over TCP, serial) is made by the first call. After an `MOGBUS_ERR_IO` or a timeout the client has closed it, and the next call connects again, so a server that restarted is picked up without any code of yours (the call that found the connection dead still fails). UDP has no connection: a stale reply is told apart by its transaction id in Modbus UDP and discarded before each request in the RTU and ASCII UDP modes.

In the RTU and ASCII modes unit 0 is the broadcast address: the frame is sent, nothing is awaited and the call returns `MOGBUS_OK`. Only writes are meaningful there, and the servers answer none.

## Threads and the server

A client may be used from any thread, but one thread at a time; use one client per thread to talk in parallel. A call blocks the calling thread until the reply or the deadline, so a client must not run on the thread that steps a server in the same process: the server would never get to answer. Run the server with @ref mogbus_server_run "mogbus_server_run()" on its own thread (stopped with the thread-safe @ref mogbus_server_stop "mogbus_server_stop()"), or talk to a server in another process. Logging stays process-wide.

## A complete example

[examples/client_function_code.c](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/client_function_code.c) runs a server with a vendor function code on a thread and calls it from a client; `make lib-check` builds and runs it against both libraries. It is walked through in [user-defined function codes](../modbus/user-defined-function-codes.md#calling-a-user-defined-code-with-the-mogbus-client). The client is also what the [Python client](../python/python-client.md) wraps.

## Where next

- [Python client](../python/python-client.md) for the same calls from Python.
- [Serving and stepping](library-serve.md) for the server side, and [lifetimes](library-lifetimes.md) for who owns what.
- [Exception codes](../modbus/exceptions.md) for what the server can answer.
