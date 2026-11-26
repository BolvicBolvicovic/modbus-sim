# `(device (fifo ...))`

Defines a FIFO queue read by Read FIFO Queue (FC 24).

## Syntax

```scheme
(fifo ADDRESS (values VALUE ...))
```

## Default

none

## Valid in

- `(device ...)`

## Behaviour

FIFO queues are separate from the coil/discrete/input/holding tables and file records. Only the FIFO
pointer addresses configured here exist: an FC 24 request for an undefined FIFO pointer returns
exception 02. If no FIFO queues are configured, FC 24 is unavailable (exception 02).

- `ADDRESS` is the FIFO pointer address, 0-65535. Each `(fifo ...)` must use a different address.
- `VALUE` is a 16-bit register value, 0-65535. Values are returned by FC 24 in the order listed. A
  FIFO may contain 0 to 31 values (an empty queue is `(fifo ADDRESS (values))`).
- Config is limited to 16 FIFO queues per device.

FC 24 reads are **non-destructive**: reading a FIFO does not change its contents or count, so
repeated reads return the same values. FIFO contents are config data (fixed for the run; not writable
via Modbus and not modified by the simulator). The response `FIFO Count` is the number of configured
values and is always at most 31. See [Modbus support](../../modbus-support.md) for the exact
request/response format and exception conditions.

## Examples

```scheme
(device (unit-id 1)
  (holding 0)
  (fifo 100 (values #x1234 #x5678 42))
  (fifo 200 (values)))                      ; empty queue: FIFO Count 0
```

## See also

- [file](file.md)
