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

`mogbus_access_t` carries the `kind`, the `space`, the first `address`, the `count` of registers or
bits, the `function_code` of the request and, for writes, the raw `values` about to be stored
(bits as 0 or 1; NULL for reads). Return 0 to continue, or a Modbus exception code (1-255) to answer
instead.

## When it runs

- Reads (FC 01, 02, 03, 04, the read half of FC 23 and the `registers` action of
  [user-defined function codes](../config/device/function-code.md)): after the range is known to be
  defined and before the table is read, so the client receives what the hook stored.
- Writes (FC 05, 06, 15, 16, 22 and the write half of FC 23): after the range is known to be defined
  and writable, before anything is stored. A nonzero return answers that exception and writes
  nothing, so [writes stay all-or-nothing](../modbus/writes.md). FC 22 reports the masked result
  that would be stored.
- FC 23 runs the write hook first, then (once the write is applied) the read hook.
- An undefined address or a read-only point answers exception 02 without calling the hook.
- The exception counts like any other the simulator answers (diagnostic counter, metrics).
- Not covered: file records (FC 20, 21), FIFO (FC 24), CANopen (FC 43/13), diagnostics and device
  identification.
- Host calls (`mogbus_device_write()`, `mogbus_device_set_value()`, the HTTP control API, machines)
  never call the hook.
- With `broadcast-execute` on, the hook runs once per device. Faults still act on the response.

## Rules

- The hook runs on the thread that steps the server. It may call the other `mogbus_device_*`
  functions on the same device. Do not block in it: the server does not serve anyone else meanwhile.
- A device has one hook; installing another replaces it. The hook is not part of a config and does
  not survive `mogbus_destroy()`.

## See also

- [C API overview](library-api.md)
- [Lifetimes and threading](library-lifetimes.md)
- [Python API reference](../python/python-api.md)
