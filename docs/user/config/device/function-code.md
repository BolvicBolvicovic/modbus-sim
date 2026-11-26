# `(device (function-code ...))`

Defines how the device answers a function code that mogbus does not implement, such as the vendor codes of commercial PLCs and meters.

## Syntax

```scheme
(function-code CODE
  (name "TEXT")                  ; optional, shown by --check
  (on "HEX PREFIX" ACTION)       ; repeatable, up to 16 cases
  ...)

(function-code CODE ACTION)      ; a bare ACTION is (on "" ACTION)
```

## Default

none: undefined function codes get exception 01 (illegal function)

## Valid in

- `(device ...)`

## Behaviour

The Modbus specification reserves 65-72 and 100-110 for user-defined codes, but any code 1-127 that
mogbus does not implement is allowed, because real products use others too.

- `CODE` is 1-127 and must not be a code mogbus implements (01-08, 0B, 0C, 0F, 10, 11, 14-18 and
  2B). At most 8 codes per device, each defined once.
- `(name "TEXT")` is up to 63 characters and appears in `--check` and `mogbus_device_info()`.
- A request is matched against the cases in the order written. A case matches when the request data
  (the bytes after the function code) starts with its prefix; the first match answers. `""` matches
  every request. The prefix is up to 16 bytes.
- A request that matches no case gets exception 03 (illegal data value).
- Hex strings (`"01 0A FF"`) take two digits per byte, upper or lower case; spaces between bytes are
  allowed and `""` is the empty string.

The actions are listed in [function code actions](function-code-actions.md).

User-defined codes go through the same processing as the built-in ones: the device tick, the
diagnostic counters of FC 08/11/12, [metrics](../../observability/http-endpoint.md), listen-only mode and
[faults](../faults/fault.md) (a `delay`, `drop`, `exception`, `corrupt` or `restart` fault applies to
a user-defined request like any other). A `sensor-disconnect` fault changes only FC 02 and 04 reads,
not user-defined codes. See [Modbus support](../../modbus/function-codes.md).

Codes are config-only: the C API cannot add them yet, but `mogbus_device_info()` lists them.

## Errors

- `function code N (0xNN) is implemented by mogbus and cannot be redefined`: pick a code that is not one of 01-08, 0B, 0C, 0F, 10, 11, 14-18, 2B.
- `expected (function-code CODE ...) with CODE 1-127`: `CODE` must be a whole number from 1 to 127.
- `needs at least one (on "HEX" ACTION) or ACTION`: a function code needs an action.
- `function code is already defined on this device`: define each code once per device; put several cases in one `(function-code ...)`.
- `too many user-defined function codes on one device (max 8)`: remove a code or move it to another device.
- `too many cases (max 16)`: merge or remove `(on ...)` cases.
- `"..." is not a list of hex bytes (two digits ...)`: a `reply` or `on` string has an odd digit count or a non-hex character.
- `too many bytes (max 252)` / `(max 16)`: a `reply` holds at most 252 bytes, an `on` prefix 16.

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

## See also

- [function code actions](function-code-actions.md)
- [Modbus support](../../modbus/function-codes.md)
