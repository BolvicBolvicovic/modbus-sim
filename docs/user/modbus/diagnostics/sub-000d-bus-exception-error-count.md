# Return Bus Exception Error Count (0x000D)

Diagnostics sub-function 0x000D returns how many exception responses the device has sent.

## Syntax

```text
Request:   08 | 000D | 0000
Response:  08 | 000D | count (2)
```

## Valid in

- all transports

## Behaviour

- Goes up for every exception response that is actually sent: address errors, value errors and
  injected `exception` faults. The counter starts at 0.
- A real exception that a `drop` fault suppresses still counts; an injected exception that is
  suppressed only goes up the no-response counter.

## Errors

| Code | When |
|---|---|
| 03 | the data word is not `0x0000` or the request is not 5 bytes long |

## Examples

```text
request:   08 000D 0000
response:  08 000D 0000
```

## See also

- [Exception codes](../exceptions.md)
- [Faults on the wire](../fault-effects.md)
- [Diagnostics (0x08)](../fc08-diagnostics.md)
