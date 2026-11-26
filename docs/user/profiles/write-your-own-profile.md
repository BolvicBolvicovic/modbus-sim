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
