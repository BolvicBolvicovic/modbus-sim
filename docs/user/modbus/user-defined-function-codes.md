# User-defined function codes

A device can answer a function code that mogbus does not implement, such as the vendor codes of commercial PLCs and meters, with a fixed reply, an echo, an exception, live register values or silence.

## Syntax

```text
Request:   CODE | request data ...
Response:  CODE | response data ...     (what the matching case defines)
Exception: CODE+80 | code (1)
```

```scheme
(function-code CODE
  (name "TEXT")
  (on "HEX PREFIX" ACTION)
  ...)
```

## Default

None: any code that is not built in gets exception 01.

## Valid in

- all transports
- every device, up to 8 codes each

## Behaviour

- `CODE` is 1 to 127 and not one of the 19 built-in codes (01-08, 0B, 0C, 0F, 10, 11, 14-18 and 2B).
  The Modbus specification reserves 65-72 and 100-110 for user-defined codes; other unused codes are
  allowed because real products use them too.
- The request data (the bytes after the function code) is compared with the cases in the order
  written; the first case whose prefix matches answers. A request that matches no case gets exception 03.
- The actions are `reply` (fixed bytes), `echo` (the request data), `exception` (a Modbus exception),
  `registers` (the live values of a range, laid out like function codes 01 to 04) and `no-response`.
  The config reference lists every action and limit.
- A user-defined code is processed like a built-in one: the request counts in the FC 08, 0x0B and
  0x0C diagnostic counters and in the [metrics](../observability/metrics/metrics.md), exception answers
  count as exceptions, listen-only mode suppresses the response, and the faults `delay`, `drop`,
  `exception`, `corrupt`, `disconnect` and `restart` apply. `sensor-disconnect` affects only
  function codes 02 and 04.
- A user-defined code changes no device state: `(registers ...)` only reads.
- The `(on ...)` patterns are config-only; a host program can define a code answered by a callback instead (below). `mogbus_device_info()` lists both kinds.

### Programmable from C and Python

When fixed replies are not enough (a status block computed from live values, a counter, a checksum over the request), the host defines the code with a callback. In C:

```c
int status(void* user, mogbus_device_t* dev, uint8_t unit_id, uint8_t code,
           const uint8_t* data, size_t len, uint8_t* reply, size_t reply_cap, size_t* reply_len);

mogbus_device_add_function_code(dev, 0x41, status, &state);
```

The callback gets the request data and fills `reply` (at most 252 bytes) and `*reply_len`. It returns 0 to answer with that reply, 1 to 255 to answer that exception, or `MOGBUS_FUNCTION_NO_RESPONSE` for silence; any other return value, or a longer reply, is answered with exception 04. In Python, @ref mogbus::core::Device::add_function_code "Device.add_function_code(code, callback)" takes `callback(request) -> bytes | int | None`: bytes are the reply, an int is the exception code and None is no response; an exception raised by the callback is printed and answered with exception 04.

- The same rules as a config code apply to the code number: 1 to 127, not built in, not defined twice on a device, and the same limit of 8 per device; a code cannot be removed. The call fails (false, `MogbusError`) otherwise.
- The request goes through the same path: unit routing, faults, listen-only, the diagnostic counters, metrics and the access log. With `broadcast-execute` on, the callback runs once per device for a broadcast (`unit_id` is 0) and its reply is dropped.
- The callback runs on the thread that steps the server and must not block. It may call the other device functions, for example to read the live values it reports (the [C tutorial](../library/library-hooks.md#a-vendor-function-code) and the [Python tutorial](../python/python-hooks.md#a-vendor-function-code) build a station status block this way).
- `mogbus_device_info()` lists such a code with an empty name.

### Calling a user-defined code with the mogbus client

mogbus also has a client, so the request side needs no third-party Modbus library. The reply data is what the server put after the function code, and an exception reply is an error with the code attached. The client is independent of the server: it only writes to its endpoint.

In C, @ref mogbus_client_request "mogbus_client_request()" sends any function code:

```c
mogbus_client_options_t o = mogbus_client_options_defaults();
o.port = 1502;
mogbus_client_t* c = NULL;
mogbus_client_open(&o, &c);

uint8_t data[] = { 0x01 };               /* request data after function code 0x41 */
uint8_t reply[252];
mogbus_request_t request = { .unit = 1, .function_code = 0x41,
                             .data = data, .data_len = sizeof(data),
                             .reply = reply, .reply_cap = sizeof(reply) };

mogbus_result_t err = mogbus_client_request(c, &request);
if (err == MOGBUS_OK)
	printf("%zu reply bytes\n", request.reply_len);
else if (err == MOGBUS_ERR_EXCEPTION)
	printf("exception %u\n", mogbus_client_last_exception(c));
else if (err == MOGBUS_ERR_TIMEOUT)
	printf("no response\n");                    /* a (no-response) action, or a dropped reply */
mogbus_client_close(c);
```

In Python, @ref mogbus::core::Client::request "Client.request(function_code, data)" returns the reply data as bytes and raises @ref mogbus::core::ModbusException "ModbusException" (with `.code`) for an exception:

```python
with mogbus.Client(port=1502, unit=1) as c:
    print(c.request(0x41, b"\x01").hex())     # 0456454e44: the (reply ...) of the example below
    try:
        c.request(0x41, b"\x09")
    except mogbus.ModbusException as e:
        print(e.code)                          # 3: no case matches
```

A complete program, [examples/client_function_code.c](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/client_function_code.c), serves a device whose function code 0x41 is answered by a callback, on a thread, and calls it with the client; `make lib-check` builds and runs it against both libraries. Without its error checks and the helper that finds a free port, it is:

```c
#include "mogbus.h"
#include <pthread.h>
#include <stdio.h>

/* Request 41 01 answers with the live value of holding register 0; anything else is exception 03. */
static int status(void* user, mogbus_device_t* dev, uint8_t unit_id, uint8_t code, const uint8_t* data,
                  size_t len, uint8_t* reply, size_t reply_cap, size_t* reply_len)
{
	double value = 0;

	if (len != 1 || data[0] != 0x01)
		return 3;
	mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &value);
	reply[0] = (uint8_t)((unsigned)value >> 8);
	reply[1] = (uint8_t)value;
	*reply_len = 2;
	return 0;
}

static void* serve(void* server) { mogbus_server_run(server); return NULL; }

int main(void)
{
	mogbus_t* ctx = NULL;
	mogbus_device_t* dev = NULL;
	mogbus_server_t* server = NULL;
	mogbus_client_t* client = NULL;
	mogbus_point_t point = { .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_U16, .value = 1234 };
	mogbus_server_options_t so = mogbus_server_options_defaults();
	mogbus_client_options_t co = mogbus_client_options_defaults();
	uint8_t ask[] = { 0x01 }, reply[252];
	mogbus_request_t request = { .unit = 1, .function_code = 0x41, .data = ask, .data_len = sizeof(ask),
	                             .reply = reply, .reply_cap = sizeof(reply) };
	pthread_t thread;

	mogbus_create(&ctx);
	mogbus_add_device(ctx, 1, "meter", &dev);
	mogbus_device_add_point(dev, &point);
	mogbus_device_add_function_code(dev, 0x41, status, NULL);
	so.port = co.port = 1502;                       /* the file asks the kernel for a free port */
	mogbus_server_create(ctx, &so, &server);
	pthread_create(&thread, NULL, serve, server);   /* the server has its own thread ... */

	mogbus_client_open(&co, &client);               /* ... the client only knows host and port */
	if (mogbus_client_request(client, &request) == MOGBUS_OK)
		printf("0x41 -> %d\n", (reply[0] << 8) | reply[1]);   /* 1234 */

	mogbus_client_close(client);
	mogbus_server_stop(server);                     /* lock-free: any thread or a signal handler */
	pthread_join(thread, NULL);
	mogbus_server_close(server);
	mogbus_destroy(ctx);
	return 0;
}
```

See @ref mogbus_device_add_function_code "mogbus_device_add_function_code()" and @ref mogbus_function_fn "mogbus_function_fn".

## Errors

| Code | When |
|---|---|
| 03 | no case matches the request data |
| 02 | a `(registers ...)` action covers an address that is not configured |
| any | the code of an `(exception N)` action |

## Examples

```scheme
(device
  (unit-id 1)
  (holding 0 (count 2) (static 7))
  (function-code #x41
    (name "Vendor info")
    (on "01" (reply "04 56 45 4E 44"))   ; request 41 01 -> 41 04 56 45 4E 44
    (on "02" (registers holding 0 2))    ; request 41 02 -> 41 04 00 07 00 07
    (on "03" (echo)))
  (function-code #x64
    (on "7E" (no-response))
    (exception 4)))                      ; any other request -> exception 04
```

```text
request:   41 01
response:  41 04 56 45 4E 44
```

Ask for it with the [mogbus client](#calling-a-user-defined-code-with-the-mogbus-client): `mogbus_client_request()` with a `mogbus_request_t` (unit 1, code 0x41, data `01`) in C, `c.request(0x41, b"\x01")` in Python; both return the reply data `04 56 45 4E 44`, and an exception answer is `MOGBUS_ERR_EXCEPTION` or @ref mogbus::core::ModbusException "ModbusException". Silence (a `(no-response)` action or a `drop` fault) is a timeout.

## See also

- [Talking to a Modbus server from C](../library/library-client.md) and [from Python](../python/python-client.md)
- [Config reference: user-defined function codes](../config/device/function-code.md)
- [Profile vendor-function-codes.scm](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/vendor-function-codes.scm)
- [Exception codes](exceptions.md)
