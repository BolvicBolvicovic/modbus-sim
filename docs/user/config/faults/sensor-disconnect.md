# `(fault (kind sensor-disconnect))`

Replaces successful FC 02 and FC 04 reads with exception 04, like a lost sensor.

## Syntax

```scheme
(fault (kind sensor-disconnect) [(percent N)] [(schedule ...)])
```

## Default

`percent 100`

## Valid in

- `(device ...)`
- every mode

## Behaviour

A successful FC 02 (discrete inputs) or FC 04 (input registers) read answers exception 04 (slave
device failure). Other function codes and error responses are unaffected, the connection stays open,
and the request is still processed normally. If a generic [`exception`](exception.md) fault also
fires, its configured code wins. Scheduling it models a persistent sensor outage.

## Examples

```scheme
(device (unit-id 1) (input 0 (static 21))
  (fault (kind sensor-disconnect)
    (schedule (step (after-ms 5000) (count 600)))))
```

## See also

- [degradation](../behaviours/degradation.md)
- [schedule](schedule.md)
