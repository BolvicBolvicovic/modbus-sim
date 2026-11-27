# Access hook

Run your own code when a Modbus client reads or writes a device: refresh values right before a read, react to a write or veto it. It is the embedded counterpart of subclassing a pymodbus datastore.

## Installing a hook

```c
static uint8_t on_access(void* user, mogbus_device_t* dev, const mogbus_access_t* a)
{
    if (a->kind == MOGBUS_ACCESS_READ && a->space == MOGBUS_HOLDING && a->address == 0)
        mogbus_device_set_value(dev, MOGBUS_HOLDING, 0, read_sensor(user));
    if (a->kind == MOGBUS_ACCESS_WRITE && a->values[0] > 1000)
        return 3;               /* illegal data value: nothing is written */
    return 0;                   /* go on */
}

mogbus_device_set_access_hook(dev, on_access, sensor);   /* NULL clears it */
```

`mogbus_access_t` carries the `kind`, the `target`, the `function_code` of the request and the fields
the target uses (below). For tables that is the `space`, the first `address`, the `count` of registers
or bits and, for writes, the raw `values` about to be stored (bits as 0 or 1; NULL for reads). Return 0
to continue, or a Modbus exception code (1-255) to answer instead.

## Targets

`target` is `MOGBUS_TARGET_TABLE` (0, the four tables) unless the request reaches one of the other
stores. The fields a target does not use are zero.

| `target` | Function codes | Fields used |
|---|---|---|
| `MOGBUS_TARGET_TABLE` | 01-06, 0F, 10, 16, 17, user-defined `registers` | `space`, `address`, `count`, `values` (writes) |
| `MOGBUS_TARGET_FILE_RECORD` | 14, 15 | `file`, `address` (first record), `count` (records), `values` (writes) |
| `MOGBUS_TARGET_FIFO` | 18 | `address` (the queue's pointer address), reads only |
| `MOGBUS_TARGET_CANOPEN` | 2B/0D | `node`, `index`, `subindex`, `address` (byte offset in the object), `count` (bytes), `data` and `data_len` (writes; `data` is NULL for reads) |

The pointers (`values`, `data`) are valid only during the call. Code that handles only tables must
check `target == MOGBUS_TARGET_TABLE`, because the other targets also reach a hook that was written
before they existed.

The host builds these stores with `mogbus_device_add_file_record()`, `mogbus_device_add_fifo()` and
`mogbus_device_add_canopen_object()`, or from a config, and changes them with the functions under
[Host functions](#host-functions).

## When it runs

- Reads (FC 01, 02, 03, 04, the read half of FC 17 and the `registers` action of
  [user-defined function codes](../config/device/function-code.md)): after the range is known to be
  defined and before the table is read, so the client receives what the hook stored.
- Writes (FC 05, 06, 0F, 10, 16 and the write half of FC 17): after the range is known to be defined
  and writable, before anything is stored. A nonzero return answers that exception and writes
  nothing, so [writes stay all-or-nothing](../modbus/writes.md). FC 16 reports the masked result
  that would be stored.
- FC 17 runs the write hook first, then (once the write is applied) the read hook; an exception
  from the read hook leaves the write applied.
- An undefined address, a read-only point or a machine-driven point answers exception 02 without
  calling the hook.
- FC 14 (Read File Record): after the whole request is validated (shape and every record defined;
  otherwise exception 03 or 02 and no hook call), the read hook runs once per sub-request in request
  order, then the response is built, so records the hook rewrote are what the client reads. A
  nonzero return answers that exception and the later sub-requests are not offered to the hook.
- FC 15 (Write File Record): after the whole request is validated, the write hook runs for every
  sub-request in order, with that sub-request's values, before anything is written. A nonzero
  return answers that exception and nothing is written; otherwise all sub-requests are written.
- FC 18 (Read FIFO Queue): after the FIFO address is known to exist, the read hook runs and the queue
  is read afterwards, so the hook can replace it with `mogbus_device_set_fifo()`.
- FC 2B/0D (CANopen General Reference): the read or write hook runs once the object exists and the
  range is inside it, before the object is read or written. A veto is answered as the extended
  failure `CE` with the SDO abort code `0x08000020` (data cannot be transferred or stored to the
  application), the failure that reports a refused service; the CANopen response has no place for a
  Modbus exception code, so any nonzero return gives the same reply (see
  [CANopen General Reference](../modbus/fc2b-0d-canopen-general-reference.md)).
- The exception counts like any other the simulator answers (diagnostic counter, metrics).
- Not covered: diagnostics and device identification.
- Host calls (`mogbus_device_write()`, `mogbus_device_set_value()`, the HTTP control API, machines)
  never call the hook.
- With `broadcast-execute` on, the hook runs once per device. Faults still act on the response.

## Host functions

These work on the stores the targets above address. They are the host side: they never call the
hook, and they work in builds without config support. Counts and values are 16-bit registers, except
CANopen bytes.

| Call | Does |
|---|---|
| `mogbus_device_add_file_record(dev, file, record, count, values, err, err_size)` | adds `count` records (1-126) starting at `record` in `file`; records of a file must not overlap and stay at or below 9999 |
| `mogbus_device_read_file_records` / `mogbus_device_write_file_records` | read or write consecutive records; fail if any record is undefined (a failed write changes nothing) |
| `mogbus_device_add_fifo(dev, address, count, values, err, err_size)` | adds a queue of up to 31 values at `address` |
| `mogbus_device_set_fifo(dev, address, count, values)` | replaces the queue's contents (0-31 values) |
| `mogbus_device_add_canopen_object(dev, node, index, subindex, length, data, err, err_size)` | adds an object of `length` bytes (1-238) for node 1-127 |
| `mogbus_device_read_canopen` / `mogbus_device_write_canopen` | read or write `count` bytes at an offset; fail outside the object, whose length never changes |

Adding returns false and fills `err` for an invalid argument, a duplicate, an overlap or a full device
(at most 16 files, 64 records per file, 16 FIFOs, 64 CANopen objects). The Python equivalents are
`add_file_record`, `read_file_records`, `write_file_records`, `add_fifo`, `set_fifo`,
`add_canopen_object`, `read_canopen` and `write_canopen` on `Device`.

```c
static uint8_t on_access(void* user, mogbus_device_t* dev, const mogbus_access_t* a)
{
    if (a->target == MOGBUS_TARGET_FIFO) {
        uint16_t queue[2] = { read_a(user), read_b(user) };
        mogbus_device_set_fifo(dev, (uint16_t)a->address, 2, queue);   /* what the client reads */
    }
    return 0;
}
```

## Rules

- The hook runs on the thread that steps the server. It may call the other `mogbus_device_*`
  functions on the same device. Do not block in it: the server does not serve anyone else meanwhile.
- A device has one hook; installing another replaces it. The hook is not part of a config and does
  not survive `mogbus_destroy()`.

## See also

- [C API overview](library-api.md)
- [Lifetimes and threading](library-lifetimes.md)
- [Python API reference](../python/python-api.md)
