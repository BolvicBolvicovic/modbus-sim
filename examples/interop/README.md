# Interoperability proof of concept

mogbus' own tests use mogbus' own encoders, so they cannot show that another implementation
understands it the same way. This example checks mogbus against an independent Modbus stack,
[pymodbus](https://github.com/pymodbus-dev/pymodbus), the library behind many Python SCADA
gateways, HMIs and test tools:

```bash
make                                          # builds ./mogbus
python3 -m venv .venv && .venv/bin/pip install 'pymodbus==3.8.6'
.venv/bin/python examples/interop/pymodbus_interop.py
```

For each network transport (`tcp`, `udp`, `rtu-tcp`, `rtu-udp`, `ascii-tcp`, `ascii-udp`) the script
starts mogbus with [`device.scm`](device.scm), sends every supported function code through
pymodbus' own framing (MBAP, RTU with CRC, ASCII with LRC) and decoding, and compares the answers
with the register map in `device.scm`:

```
check                                     tcp       udp       rtu-tcp   rtu-udp   ascii-tcp ascii-udp
FC 01 read coils                          pass      pass      pass      pass      pass      pass
FC 03 read holding registers              pass      pass      pass      pass      pass      pass
...
FC 18 read FIFO queue                     known     known     known     known     known     known
FC 2B read device identification          pass      pass      pass      pass      pass      pass
FC 41 user-defined code: fixed reply      pass      pass      pass      pass      pass      pass
FC 41 user-defined code: live registers   pass      pass      pass      pass      pass      pass
known client issue, not counted: read FIFO queue: pymodbus 3.8.6 FC 24 decoder bug
all checks passed
```

The two `FC 41` rows cover a user-defined function code: `device.scm` defines `0x41` with
`(function-code ...)`, and the script sends it through pymodbus' custom PDU support (a `ModbusPDU`
subclass for the request, another registered with `client.register()` for the response). One case
answers a fixed reply, the other the live value of holding register 0, so the check shows that an
independent client decodes both over every framing, including the RTU frame length it derives from
the response's byte count.

The `data type` rows read 64-bit integers, float64, byte-swapped float32/s64 and string registers
from `device.scm` and decode them with pymodbus' `convert_from_registers` (`DATATYPE.UINT64`,
`INT64`, `FLOAT64`, `STRING`, `word_order="little"` for low-first). pymodbus has no byte-order
option, so the byte-swapped layouts are decoded by swapping the bytes of every register first.

It exits non-zero on any mismatch, so it can run in CI. Pass mode names to test a subset.

## What it already found

The first run found a bug, but in the client: pymodbus 3.8.6 decodes a Read FIFO Queue response
with `count - 4` values (and encodes the byte length as the FIFO count), so a spec-correct reply
`18 00 08 00 03 00 01 00 02 00 03` (byte count 2 + 2·N, FIFO count N = 3) decodes as empty. The
script lists such client-side issues as `known`, with the evidence, instead of counting them. Two
other mismatches during development were misuse of the client API (pymodbus' `FileRecord` takes
`record_length` in bytes), which is the other thing interop testing finds: places where two
reasonable readings of the spec or an API differ.

## Testing a commercial PLC, SCADA or HMI client

The same method applies to products you cannot script directly:

1. **Model the device.** Write a config with the register map the product expects (or copy a
   profile), including the identification objects and a fault-free first device.
2. **Run mogbus where the product can reach it.** Bind `0.0.0.0` (or run the container image), on
   the transport the product uses. For serial products, `rtu-device` / `ascii-device` on a USB
   serial adapter, or `rtu-pty` with a virtual COM port bridge.
3. **Point the product at it** and configure the tags/points it should poll and write.
4. **Watch both sides.** `mogbus -v --log-format json` logs every request (unit, function code,
   result, exception, fault) and `/metrics` counts them (see
   [metrics](../../docs/user/observability/metrics/metrics.md)), so a checklist per function code can be
   ticked from the logs: the product polled FC 03 on unit 1 and got `ok`, a write of tag X arrived
   as FC 06 at address N with value V, an alarm appeared when a `sensor-disconnect` fault fired.
5. **Inject faults** (`delay`, `drop`, `exception`, `corrupt`, `disconnect`) and check the
   product's timeouts, retries and alarms.

Products often use vendor-specific function codes or quirks on top of the standard ones. Model
those with `(function-code ...)` (see the
[config reference](../../docs/user/config/device/function-code.md)
and [`profiles/vendor-function-codes.scm`](../../profiles/vendor-function-codes.scm)): a fixed
reply, an echo, an exception, live register values or silence per request prefix. The logs and
`/metrics` then show the vendor codes like any other.
