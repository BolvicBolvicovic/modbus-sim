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
- Codes are config-only: the C API cannot define them, but `mogbus_device_info()` lists them.

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
