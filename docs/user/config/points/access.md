# `(holding (access ...))`

Makes a coil or holding point refuse every Modbus write while clients can still read it.

## Syntax

```scheme
(access read-write|read-only)
```

## Default

`read-write`

## Valid in

- `(coil ...)`
- `(holding ...)`

## Behaviour

`read-only` models a status word or a counter in a real device. A write (FC 05, 06, 15, 16, 22 or the
write part of FC 23) that touches any address of a read-only point fails as a whole with exception 02,
exactly like a write to an undefined address: nothing is written, and a ramp, sine or lag on the point
keeps moving because clients cannot freeze it. [Machines](../machines/index.md), `(set! ...)` and the
host side of the [C library](../../library/library-start.md) and the HTTP control API (`PUT` of raw values and
`/value`) can still change the point. `(access ...)` is rejected on `input` and `discrete` points,
which are always read-only for clients.

## Errors

- `(access ...): expected one of: read-write, read-only`: the access is `read-write` or `read-only`.
- `input registers are always read-only for clients: remove (access ...)` / `discrete inputs are always ...`: only `coil` and `holding` points take `(access ...)`; remove it.

## Examples

```scheme
(device (unit-id 1)
  (holding 0 (access read-only) (static #x0001))     ; status word
  (holding 10 (access read-only) (ramp (from 0) (to 65535) (step 1) (every-ms 1000) (mode wrap))))
```

A client write is refused:

```bash
./tools/modbus_probe.py write-reg 0 5   # exception 0x02: the point is read-only
```

## See also

- [client writes](../behaviours/client-writes.md)
