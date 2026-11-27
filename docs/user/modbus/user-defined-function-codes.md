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
- The callback runs on the thread that steps the server and must not block. It may call the other device functions, for example to read the live values it reports (the [C tutorial](../library/library-hooks.md#a-vendor-function-code) and the [Python tutorial](../python/python-process.md#a-vendor-function-code) build a station status block this way).
- `mogbus_device_info()` lists such a code with an empty name.

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

## See also

- [Config reference: user-defined function codes](../config/device/function-code.md)
- [Profile vendor-function-codes.scm](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/vendor-function-codes.scm)
- [Exception codes](exceptions.md)
