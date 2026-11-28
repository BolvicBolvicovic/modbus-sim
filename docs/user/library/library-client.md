# Talking to a Modbus server from C

Use the mogbus client to read and write a Modbus server (a real device, the `mogbus` binary or a server of your own process) with one call per function code.

The server side of the library simulates devices. The client side does the opposite: it is a Modbus master that sends requests to an endpoint and returns the replies. A @ref mogbus_client_t "mogbus_client_t" is independent of every @ref mogbus_t "mogbus_t" and of every server: it writes to an endpoint over the wire (or a serial line) and shares no memory with the server, even when the server is in the same process on another thread.

## Open a client

Start from @ref mogbus_client_options_defaults "mogbus_client_options_defaults()" (TCP to `127.0.0.1:1502`, 1000 ms timeout, no retries), change what you need and open the client:

```c
mogbus_client_options_t o = mogbus_client_options_defaults();
o.mode       = MOGBUS_MODE_RTU_TCP;   /* or TCP, UDP, RTU_UDP, ASCII_TCP, ASCII_UDP */
o.host       = "192.168.1.20";        /* an IP address or a host name */
o.port       = 502;
o.timeout_ms = 500;
o.retries    = 2;                     /* extra attempts after a timeout */
o.byte_timeout_ms = 50;               /* stream modes: longest gap inside one reply (0 = off) */

mogbus_client_t* c = NULL;
if (mogbus_client_open(&o, &c) != MOGBUS_OK)
	fprintf(stderr, "client: %s\n", mogbus_last_error(NULL));
```

| Mode | Fields |
|---|---|
| `MOGBUS_MODE_TCP`, `UDP`, `RTU_TCP`, `RTU_UDP`, `ASCII_TCP`, `ASCII_UDP` | `host`, `port` |
| `MOGBUS_MODE_RTU_DEVICE`, `ASCII_DEVICE` (a serial line) | `path`, `baud`, `data_bits`, `parity`, `stop_bits` |

The other modes are for servers and are refused.

### Host names

`host` is an IP literal or a host name (letters, digits, `-`, `_` and `.`). A name is resolved with `getaddrinfo` at connect time, not when the client is opened, and again on every reconnect, so a changed address is picked up. IPv4 and IPv6 addresses are both used: the client tries them in the resolver's order, giving each an equal share of what is left of `timeout_ms`, and the first that accepts the connection wins. Resolution blocks the calling thread for as long as the system resolver takes (its own timeout, which `timeout_ms` cannot cut short), and the time it takes counts against the deadline. A name that does not resolve fails the connecting call with `MOGBUS_ERR_IO` (`cannot resolve host 'NAME'`). IP literals are never resolved.

### Byte timeout

`timeout_ms` bounds the whole wait for a reply. `byte_timeout_ms` (0 = off, the default) additionally bounds the silence inside a reply on the stream modes (TCP, RTU and ASCII over TCP, serial lines, TLS): once the first byte of a reply has arrived, a gap longer than `byte_timeout_ms` before the next byte fails the call with `MOGBUS_ERR_TIMEOUT` (`reply from unit N stalled: no byte for N ms`), without waiting for `timeout_ms`. It never applies before the first byte (a slow device is `timeout_ms`'s business) and not to the UDP modes, where a reply is one datagram. It is a timeout like any other: the connection is closed, and the call is retried when `retries` allows.

### TLS

Set `tls_enabled` to talk Modbus TCP over TLS (`MOGBUS_MODE_TCP` only; port 0 then means 802). The handshake runs when the connection is made, within `timeout_ms`, and verifies the server by default:

```c
mogbus_client_options_t o = mogbus_client_options_defaults();
o.tls_enabled   = true;
o.port          = 802;
o.host          = "plc.example";          /* also the name the certificate must carry (and the SNI) */
o.tls_ca_file   = "ca.pem";               /* the CA that signed the server certificate */
o.tls_cert_file = "client.pem";           /* only when the server requires a client certificate */
o.tls_key_file  = "client.key";           /* NULL when the key is in tls_cert_file */
```

| Field | Meaning |
|---|---|
| `tls_ca_file` | PEM file of the trusted CA(s); NULL uses the system trust store |
| `tls_cert_file`, `tls_key_file` | the client certificate and key to present (mutual TLS) |
| `tls_server_name` | the name the server certificate must carry, also sent as SNI; NULL uses `host`. An IP literal is checked against the certificate's IP addresses |
| `tls_verify` | true by default; false accepts any certificate and any name (no protection against an impostor) |

TLS 1.2 and 1.3 are used. A failed handshake (an untrusted or expired certificate, a name that does not match, a server that speaks plain Modbus) fails the connecting call with `MOGBUS_ERR_IO` and a message such as `TLS handshake with 127.0.0.1:802 failed: ... certificate verify failed (hostname mismatch)`. When the server requires a certificate and the client has none, TLS 1.3 reports it on the first read, so the failure comes with that call. Bad or missing files make @ref mogbus_client_open "mogbus_client_open()" fail. A library built without TLS (`WITH_TLS=0`, and every Windows build) fails the open with `MOGBUS_ERR_UNSUPPORTED`.

@ref mogbus_client_open "mogbus_client_open()" only checks the options and allocates; the connection is made by the first call. Call @ref mogbus_client_connect "mogbus_client_connect()" to connect at once and learn early that the endpoint is not there.

Because there is no client yet to own the message of a failed open, it is kept per thread, like the message of @ref mogbus_probe "mogbus_probe()": read it with `mogbus_last_error(NULL)`. Every later call reports its error through its return value, and the client keeps the text: @ref mogbus_client_last_error "mogbus_client_last_error()".

## Read and write

Every call takes the unit id of the server to ask and returns a @ref mogbus_result_t "mogbus_result_t". Reads fill a buffer you pass in: bits as one byte each (0 or 1), registers as raw 16-bit values.

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
| @ref mogbus_client_read_exception_status "mogbus_client_read_exception_status()" | 07 |
| @ref mogbus_client_diagnostics "mogbus_client_diagnostics()" | 08 |
| @ref mogbus_client_get_comm_event_counter "mogbus_client_get_comm_event_counter()" | 0B |
| @ref mogbus_client_get_comm_event_log "mogbus_client_get_comm_event_log()" | 0C |
| @ref mogbus_client_report_server_id "mogbus_client_report_server_id()" | 11 |
| @ref mogbus_client_read_file_records "mogbus_client_read_file_records()" | 14 |
| @ref mogbus_client_write_file_records "mogbus_client_write_file_records()" | 21 (0x15) |
| @ref mogbus_client_read_fifo "mogbus_client_read_fifo()" | 24 (0x18) |
| @ref mogbus_client_canopen_request "mogbus_client_canopen_request()" | 43 (MEI 13) |
| @ref mogbus_client_read_device_id "mogbus_client_read_device_id()" | 43 (MEI 14, basic objects) |
| @ref mogbus_client_read_device_identification "mogbus_client_read_device_identification()" | 43 (MEI 14, any read code, follows up "more") |
| @ref mogbus_client_request "mogbus_client_request()" | any, including user-defined codes |

The quantities are the protocol's limits (2000 bits or 125 registers per read, 1968 coils or 123 registers per write); a call over the limit fails with `MOGBUS_ERR_INVALID` before anything is sent. The client checks the reply: a write must echo its request, a read must return the number of values asked for, and every typed call below checks the lengths and echoes of its reply, otherwise the call fails with `MOGBUS_ERR_FRAME`.

## Diagnostics, logs, files, FIFOs, CANopen and identification

The rarer function codes have typed calls too; the ones that return data fill buffers you pass in, with a capacity and a length out parameter.

```c
uint8_t  status;
mogbus_client_read_exception_status(c, 1, &status);                         /* FC 07 */

uint8_t  data[250];  size_t n;
mogbus_client_diagnostics(c, 1, 0x0000, (uint8_t[]){ 0x12, 0x34 }, 2,       /* FC 08: echo */
                          data, sizeof(data), &n);

uint16_t st, events, messages;
mogbus_client_get_comm_event_counter(c, 1, &st, &events);                   /* FC 0B */
uint8_t  log[64];
mogbus_client_get_comm_event_log(c, 1, &st, &events, &messages, log, sizeof(log), &n);  /* FC 0C */
mogbus_client_report_server_id(c, 1, data, sizeof(data), &n);               /* FC 11 */

uint16_t a[2], b[1];
mogbus_client_file_record_t reads[] = { { 1, 0, 2, a }, { 1, 4, 1, b } };    /* FC 14, one reply */
mogbus_client_read_file_records(c, 1, reads, 2);
mogbus_client_write_file_records(c, 1, reads, 1);                           /* FC 15 */

uint16_t fifo[31];
mogbus_client_read_fifo(c, 1, 100, fifo, 31, &n);                           /* FC 18 */
```

- **FC 08** returns the data after the sub-function and checks that the server echoed the sub-function (`MOGBUS_ERR_FRAME` otherwise). The data of sub-function `0x0000` is echoed back and may be longer than 2 bytes; the stream framing of the RTU modes allows for that.
- **FC 11** returns the device specific report as bytes: for a mogbus device the server id, the run indicator (`0xFF` on) and the additional data.
- **FC 14 and 15** take an array of @ref mogbus_client_file_record_t "mogbus_client_file_record_t" (file, first record, number of records and a buffer of that many 16-bit values: filled by a read, written by a write). A read accepts up to 35 sub-requests and returns all values in one reply; a write must fit the 251 bytes of a request and is all or nothing.
- **FC 24** returns at most 31 values.
- **FC 43/13 (CANopen)**: @ref mogbus_client_canopen_request "mogbus_client_canopen_request()" sends the raw request bytes after `2B 0D` (control byte, reserved 0, optional network id and data type, node, index, subindex, start, count, write data) and returns the raw reply bytes after `2B 0D`; the CANopen payload is yours to build and decode. A failure of the CANopen service (`AB FF len 0D AE|CE ...`, for example an object that does not exist) is a `MOGBUS_ERR_EXCEPTION` with @ref mogbus_client_last_exception "mogbus_client_last_exception()" `0xFF`, and the reply buffer holds `FF len 0D AE|CE payload` so you can read the failure code. See [CANopen general reference](../modbus/fc2b-0d-canopen-general-reference.md).
- **FC 43/14 (identification)**: @ref mogbus_client_read_device_identification "mogbus_client_read_device_identification()" takes the read code (1 basic, 2 regular, 3 extended, 4 one object) and the first object id, and repeats the request while the server says more objects follow (`more follows`), so a device with many or large objects is read completely into one buffer of `[id][length][value]` triples, together with the conformity level. A reply that does not make progress (a next object that is not after the last one returned) fails with `MOGBUS_ERR_FRAME` instead of looping. @ref mogbus_client_read_device_id "mogbus_client_read_device_id()" is the basic read (code 1, object 0) and follows up as well.

In every typed call a Modbus exception is `MOGBUS_ERR_EXCEPTION` with the code in @ref mogbus_client_last_exception "mogbus_client_last_exception()", as for the other calls.

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
