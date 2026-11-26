# `(fault (kind delay))`

Holds back the response of a device by a fixed time, to test client timeouts.

## Syntax

```scheme
(fault (kind delay) (percent N) (delay-ms N))
```

## Default

`percent 100`

## Valid in

- `(device ...)`

## Behaviour

Each request that fires the fault is answered `delay-ms` later. The server never blocks while waiting.

## Errors

`delay-ms` above 60000 is rejected.

## Examples

```scheme
(device
  (unit-id 1)
  (fault (kind delay) (percent 25) (delay-ms 750)))
```

## See also

- [server mode](server-mode.md)
