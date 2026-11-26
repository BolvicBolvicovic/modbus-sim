# Read Exception Status (0x07)

Function code 0x07 returns one status byte that is configured per device.

## Syntax

```text
Request:   07
Response:  07 | status (1)
Exception: 87 | code (1)
```

```scheme
(exception-status N)
```

## Default

`(exception-status 0)`

## Valid in

- all transports
- every device

## Behaviour

- The request is the function code only. The response is the device's `exception-status` byte, 0 to 255.
- The status is a fixed setting: nothing changes it while the simulator runs.
- A request with any extra data is rejected.

## Errors

| Code | When |
|---|---|
| 03 | the request has data after the function code |

## Examples

With `(exception-status 5)`:

```text
request:   07
response:  07 05
```

```python
print(client.read_exception_status().status)
```

## See also

- [Config reference: device entries](../config/device/index.md)
- [Exception codes](exceptions.md)
