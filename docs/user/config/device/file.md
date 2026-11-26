# `(device (file ...))`

Defines the 16-bit register records read by Read File Record (FC 20) and written by Write File Record (FC 21).

## Syntax

```scheme
(file FILE_NO
  (record REC_NO VALUE ...)
  (record REC_NO VALUE ...))
```

## Default

none

## Valid in

- `(device ...)`

## Behaviour

File records are separate from the coil/discrete/input/holding tables. Only the ranges configured here
exist and are writable/readable: FC 20/21 requests targeting an undefined file or any undefined record
return exception 02. If no file records are configured, any FC 20 read or FC 21 write is unavailable
(exception 02). FC 21 updates in-memory values (readable back via FC 20) until the simulator restarts;
it does not modify the config file and does not create new files/records.

- `FILE_NO` is the file number, 1-65535 (`0x0001-0xFFFF`). A device can have several `(file ...)`
  entries (and several `(record ...)` entries inside a file).
- `REC_NO` is the starting record number, 0-9999 (`0x270F`).
- Each `VALUE` is a 16-bit register value, 0-65535. A record defines that many consecutive registers
  starting at `REC_NO` (one register per value, in order), and may contain at most 126 values
  (`DEVICE_MAX_RECORD_VALUES`).
- Record ranges in the same file must not overlap. Record ranges must fit within record 9999.
- Config is limited to 16 files per device and 64 record ranges per file.

A client request may read or write any contiguous range that is fully defined by configuration;
requesting an undefined file or any undefined record in the range returns exception 02 (illegal data
address), like undefined table addresses. Multiple sub-requests in one FC 20/21 request are supported
and answered in order (FC 21 echoes the request on success); the request is rejected as a whole if any
sub-request is invalid or unavailable/unwritable, or (for FC 20) if the response would not fit in a
Modbus PDU. FC 21 is all-or-nothing: no records are modified if any sub-request fails validation. See
[Modbus support](../../modbus-support.md) for the exact request/response format, semantics, and
exception conditions.

## Examples

```scheme
(device (unit-id 1)
  (holding 0)
  (file 1
    (record 0 #x1234 #x5678)   ; file 1, records 0-1
    (record 10 100 200 300))   ; file 1, records 10-12
  (file 4
    (record 2 7)))
```

## See also

- [fifo](fifo.md)
- [Modbus support](../../modbus-support.md)
