# `(function-code (on ...))` actions

Lists what a matched user-defined function code can answer: `reply`, `echo`, `exception`, `registers` or `no-response`.

## Syntax

```scheme
(reply "HEX")
(echo)
(exception N)
(registers SPACE ADDRESS COUNT)
(no-response)
```

## Valid in

- `(function-code ...)`

## Behaviour

| Action | Response data (after the function code) |
|---|---|
| `(reply "HEX")` | The bytes given, up to 252 |
| `(echo)` | The request data, unchanged |
| `(exception N)` | Exception response with code `N`, 1-255 |
| `(registers SPACE ADDRESS COUNT)` | A byte count and the current values of the range, laid out like FC 01-04 (bits packed least significant first for `coil` and `discrete`, big-endian 16-bit words for `input` and `holding`). `SPACE` is `coil`, `discrete`, `input` or `holding`; `COUNT` is 1-125 registers or 1-2000 bits. If any address in the range is undefined the answer is exception 02 |
| `(no-response)` | Nothing is sent |

A [`fail-after-ms`](../behaviours/degradation.md) point inside a `registers` range answers exception 04.

## Errors

- `expected (reply "HEX"), (echo), (exception N), (registers SPACE ADDRESS COUNT) or (no-response)`: the action is misspelled or has the wrong number of arguments.
- `the space must be coil, discrete, input or holding`: fix the space name in `(registers ...)`.
- `expected ADDRESS 0-65535 and COUNT 1-125 that stay below 65536`: `COUNT` is at most 125 registers (2000 for `coil` and `discrete`) and the range must end by address 65535.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (count 2) (static 7))
  (function-code #x41 (registers holding 0 2)))   ; request 41 -> 41 04 00 07 00 07
```

## See also

- [function-code](function-code.md)
