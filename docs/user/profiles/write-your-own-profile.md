# Write your own profile

Copy the closest profile, fill in your device's register map, check it and run it.

If you already have the register map as a CSV table or a pymodbus simulator file, start from
[`tools/mogbus_import.py`](../import/import-overview.md) instead of typing it.

## Steps

1. Copy the closest profile: `cp profiles/power-meter.scm my-meter.scm`.
2. Open your device's register table. For each entry you care about, write one line:
   - which table: `coil`, `discrete`, `input` or `holding`
   - the address as sent on the wire (see the [config reference](../config/points/index.md))
   - the `type` and `word-order` if it is not a plain 16-bit unsigned value
   - the behaviour: `(static ...)` or `(ramp ...)`
3. Check it: `./mogbus --check my-meter.scm`.
4. Run it and read the values with `tools/modbus_probe.py` or your own client.

## A walkthrough

A config has one `(server ...)` section and one or more `(device ...)` sections.

### The server section

```scheme
(server (mode tcp) (port 1502) (bind "127.0.0.1"))

(device (name "Placeholder") (unit-id 1) (holding 0 (static 1)))
```

`mode tcp` is Modbus TCP, `port` is where it listens (1502 avoids the privileged port 502) and
`bind` is the interface. `127.0.0.1` keeps the simulator reachable from your own machine only; use
`"0.0.0.0"` when another machine or a container port mapping must reach it. Other modes
(`rtu-tcp`, `udp`, serial, ...) are described in [Server](../config/server/server.md). The second
line is only there so the example is a complete config.

### A device with points of different kinds

```scheme
(server (mode tcp) (port 1502))

(device
  (name "Boiler controller")
  (unit-id 3)

  ;; read-only measurements (function code 04)
  (input 0 (type s16) (ramp (from 600) (to 640) (step 2) (every-ms 1000) (mode bounce)))
  (input 1 (type float32) (static 3.5))

  ;; a writable setting (function codes 03, 06, 16)
  (holding 0 (static 650))

  ;; a text register block: 8 registers hold up to 16 ASCII characters
  (holding 10 (type string) (length 8) (static "BOILER-7"))

  ;; bits
  (coil 0 (static off))
  (discrete 0 (static on)))
```

- `(unit-id 3)` is the address clients put in the request. Requests for another unit get no answer.
- `(input 0 ...)` is an input register. `s16` is a signed 16-bit value; a `ramp` moves between `from`
  and `to` by `step` every `every-ms`, and `bounce` turns around at the ends. A temperature of
  62.0 C scaled by 0.1 is written as `620`.
- `(input 1 ...)` is a `float32`, which uses two registers (addresses 1 and 2), so the next input
  point must start at address 3 or later. Overlapping points are rejected by `--check`.
- `(holding 0 (static 650))` has no `type`, so it is an unsigned 16-bit register. It starts at 650
  and a client can change it.
- The `string` point stores text in `length` registers.
- `coil` and `discrete` are single bits; they take `on`/`off`.

Save it as `boiler.scm`, then `./mogbus --check boiler.scm` and `./mogbus boiler.scm`. Reading
`holding 0` returns 650; reading an address you did not define, such as `input 100`, returns
exception 02. The full list of point keys is in the [config reference](../config/points/index.md).

## Tips

- **Scaling.** Devices usually send whole numbers, scaled: 21.5 C as `215` means 0.1 C units. Write
  the *raw* number in the config, and put the scale in a `(note ...)` or a comment.
- **Realistic noise.** Use a small `bounce` ramp around the normal value, for example
  `(from 228) (to 232) (step 0.5)` for a mains voltage.
- **Vendor function codes.** If the product also uses its own function codes, define them with
  `(function-code ...)`; see `vendor-function-codes.scm` and
  [User-defined function codes](../modbus/user-defined-function-codes.md).
- **Counters.** A `wrap` ramp with a large range and a small step makes a counter that keeps growing
  (energy, totalizers).
- **Start-up behaviour.** A `hold` ramp climbs once and stays, good for "motor accelerating".
- **Unknown addresses fail.** If a client polls an address your profile does not define, it gets
  exception 02. Define every address your client reads, even as `(static 0)`.
- **Word order.** If a float32 or u32 value looks wrong (huge, tiny or garbage), try
  `(word-order low-first)` or `--low-first` in the probe tool. If it is still wrong, the bytes inside
  each register may be swapped: try `(byte-order little)` or `--byte-order little`. The probe tool
  also reads `--as u64`, `s64`, `float64` and `string`.
- **Test failures.** Add `(fault (kind drop) (percent 10))` to any profile to test how your client
  handles timeouts, or `(kind delay)` for slow answers.

## See also

- [Profile catalog](profile-catalog.md)
- [Using a profile](using-profiles.md)
- [Faults on the wire](../modbus/fault-effects.md)
