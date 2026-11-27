# Writes are all-or-nothing

A write either changes every address it names or none of them. This page lists what can be written, what refuses a write, and what a write sets in motion.

## What can be written

Only coils ([0x05](fc05-write-single-coil.md), [0x0F](fc0f-write-multiple-coils.md)) and holding
registers ([0x06](fc06-write-single-register.md), [0x10](fc10-write-multiple-registers.md),
[0x16](fc16-mask-write-register.md), [0x17](fc17-read-write-multiple-registers.md)) can be written, plus
configured file records ([0x15](fc15-write-file-record.md)). Modbus has no function code that writes
input registers or discrete inputs, so a write to an address that exists only as an input register or
discrete input is exception 02: the target table has no such point.

## What refuses a write

A write that touches any of these fails as a whole with exception 02, leaves every value unchanged
and counts as an exception:

- an address that is not configured;
- a machine-owned point, including a machine state register: it is read-only to Modbus clients;
- a point configured `(access read-only)`.

For FC 0x16 and the write range of FC 0x17 the check happens before anything is written or read.
Read/Write Multiple Registers also validates the read range first, so an unavailable or malformed
request changes no register. Write File Record validates every sub-request first, so an unavailable
or malformed request changes no record, and it can update only configured file-record ranges.

## What a write triggers

- A ramp that is written becomes a static value.
- After the raw values are applied, matching machine `on-write` handlers run in the same request.
  They do not fire transitions or advance the machine.
- Values last until the simulator restarts. The config file is never changed.

The host side (the C library, the control API, machines) writes with `device_write` semantics:
read-only points can be written there. See [host writes in the C tutorial](../library/library-simulate.md) and the
[control API](../observability/api/control-api.md).

## See also

- [Exception codes](exceptions.md)
- [Config reference: data points](../config/points/index.md)
