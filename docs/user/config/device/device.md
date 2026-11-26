# `(device ...)`

Declares one simulated Modbus device with its own unit id, data and behaviour.

## Syntax

```scheme
(device
  (unit-id N)
  [(name "Text")] [(note "Text")]
  [points, faults, machines and the entries of this area] ...)
```

## Default

none: a file needs at least one device.

## Valid in

- top level, repeatable

## Behaviour

Unit ids must be unique across devices. The device holds four Modbus tables
([points](../points/point.md)), optional [faults](../faults/fault.md), [machines](../machines/machine.md)
and the data answered by the less common function codes.

| Entry | What it does |
|---|---|
| [`(unit-id N)`](unit-id.md) | Modbus unit id (slave address), required |
| [`(name "Text")`](name.md) | Name shown in logs and `--check` |
| [`(note "Text")`](note.md) | A comment kept for humans |
| [`(exception-status N)`](exception-status.md) | Byte returned by FC 07 |
| [`(server-id N)`](server-id.md), [`(run-indicator N)`](run-indicator.md), [`(server-id-additional-data "HEX")`](server-id-additional-data.md) | 0x11 Report Server ID |
| [`(com-status N)`](com-status.md), [`(com-event N)`](com-event.md) | 0x0B and 0x0C |
| [`(vendor-name ...)` and the other identification objects](identification.md) | 0x2B / MEI 0x0E |
| [`(device-id-object ID "Text")`](device-id-object.md) | Extended identification objects |
| [`(canopen-object ...)`](canopen-object.md) | 0x2B / MEI 0x0D SDO data |
| [`(file ...)`](file.md) | 0x14/0x15 file records |
| [`(fifo ...)`](fifo.md) | 0x18 FIFO queues |
| [`(function-code ...)`](function-code.md) | User-defined function codes |

## Examples

```scheme
(device
  (name "Power meter")
  (unit-id 1)
  (holding 0 (type u16) (static 230)))
```

## See also

- [unit-id](unit-id.md)
- [points](../points/point.md)
