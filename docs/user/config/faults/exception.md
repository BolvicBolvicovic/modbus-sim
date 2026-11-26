# `(fault (kind exception))`

Replaces the response with a Modbus exception.

## Syntax

```scheme
(fault (kind exception) (code N) [(percent N)])
```

## Default

`percent 100`

## Valid in

- `(device ...)`
- every mode

## Behaviour

`code` is required: one of 1-8, 10 or 11 (decimal); 9 is reserved. The response becomes
`[FC|0x80][code]`. The request is still processed normally. A fired exception wins over a
[`sensor-disconnect`](sensor-disconnect.md) read. It is how a client sees the codes `04`, `05`, `06`,
`07`, `08` and `0A` that the simulator never produces by itself; see
[Modbus support](../../modbus-support.md).

## Errors

- `missing required option (code ...)`: `(kind exception)` needs `(code N)`.
- `code is only valid for kind exception`: remove `(code ...)` from other fault kinds.
- `code 9 is reserved`: use one of 1-8, 10 or 11.

## Examples

```scheme
(device (unit-id 1) (holding 0 (static 1))
  (fault (kind exception) (code 6) (percent 100)))   ; server busy
```

Read it from a client:

```bash
./tools/modbus_probe.py holding 0 1   # exception 0x06
```

## See also

- [sensor-disconnect](sensor-disconnect.md)
- [fault](fault.md)
