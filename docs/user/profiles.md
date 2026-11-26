# Device profiles

A profile is just a config file for one kind of device. The ones in [`profiles/`](../../profiles)
are generic examples of common device types. They are **not** copies of any one manufacturer's
register map; adapt the addresses and units to the device you want to imitate.

## Use a profile

```bash
./mogbus profiles/power-meter.scm
./tools/modbus_probe.py input 0 2 --as float32      # -> [228.0]
```

Every profile starts with a comment that lists its register map, so you can read the file
instead of a manual.

## The catalog

| Profile | Unit | What it offers |
|---|---|---|
| `minimal.scm` | 1 | holding 0 fixed, holding 1 counts 0-100 |
| `modbus-udp.scm` | 1 | same registers served over Modbus UDP (`mode udp`), with a small delay fault |
| `ascii-udp.scm` | 1 | same registers served as ASCII frames over UDP (`mode ascii-udp`), with a small delay fault |
| `rtu-udp.scm` | 1 | same registers served as RTU frames over UDP (`mode rtu-udp`), with a small delay fault |
| `power-meter.scm` | 1 | 3 voltages, 3 currents, power, frequency, power factor, energy (float32 input registers); 2 settings; reset coil; alarm |
| `temperature-controller.scm` | 1 | process value (s16, 0.1 C), output %, setpoint and mode (writable), run coil, 2 alarms |
| `heater.scm` | 1 | temperature that lags a writable setpoint with gaussian noise, ambient temperature (sine), setpoint |
| `vfd-drive.scm` | 1 | status, frequency and current that climb and hold, DC voltage, temperature, 32-bit energy; control word, setpoint, run and reset coils |
| `motor.scm` | 1 | scripted state machine: idle/starting/running/tripped, timed ramp and random trip |
| `flow-meter.scm` | 1 | flow rate and temperature (float32), 32-bit totalizer, reset coil, empty-pipe alarm |
| `io-module.scm` | 1 | 16 digital inputs, 16 digital outputs, 8 analog inputs (4-20 mA), 4 analog outputs |
| `slow-sensor.scm` | 1 | temperature sensor; 30% of answers arrive 1.5 s late |
| `unreliable-meter.scm` | 1 | energy counter; 20% of answers never arrive |
| `two-devices.scm` | 1, 2 | two devices on one port |
| `vendor-function-codes.scm` | 1 | status, serial number, setpoint, 2 live inputs; user-defined function codes 0x41 (fixed reply, live registers, echo) and 0x64 (acknowledge, exception, no response) |

Every profile except the three UDP ones uses Modbus TCP, so `tools/modbus_probe.py` (a Modbus TCP
client) can read it. To serve any profile over another transport, change its `(server ...)` line,
for example `(server (mode rtu-pty))` or `(server (mode udp))`; the devices stay the same.

## Try things

```bash
# watch a value move
./tools/modbus_probe.py --watch 1 input 0 1 --as s16

# write a setpoint, then read it back (temperature controller)
./tools/modbus_probe.py write-reg 0 250
./tools/modbus_probe.py holding 0 1 --as s16

# talk to the second device of two-devices.scm
./tools/modbus_probe.py --unit 2 input 0 2 --as float32

# see every request the server gets
./mogbus -v profiles/slow-sensor.scm
```

## Write your own profile

1. Copy the closest profile: `cp profiles/power-meter.scm my-meter.scm`.
2. Open your device's register table. For each entry you care about, write one line:
   - which table: `coil`, `discrete`, `input` or `holding`
   - the address as sent on the wire (see [config-reference](config-reference.md#data-points))
   - the `type` and `word-order` if it is not a plain 16-bit unsigned value
   - the behaviour: `(static ...)` or `(ramp ...)`
3. Check it: `./mogbus --check my-meter.scm`.
4. Run it and read the values with `tools/modbus_probe.py` or your own client.

Tips:

- **Scaling.** Devices usually send whole numbers, scaled: 21.5 C as `215` means 0.1 C units.
  Write the *raw* number in the config, and put the scale in a `(note ...)` or a comment.
- **Realistic noise.** Use a small `bounce` ramp around the normal value, for example
  `(from 228) (to 232) (step 0.5)` for a mains voltage.
- **Vendor function codes.** If the product also uses its own function codes, define them with
  `(function-code ...)`; see `vendor-function-codes.scm`.
- **Counters.** A `wrap` ramp with a large range and a small step makes a counter that keeps
  growing (energy, totalizers).
- **Start-up behaviour.** A `hold` ramp climbs once and stays, good for "motor accelerating".
- **Unknown addresses fail.** If a client polls an address your profile does not define, it gets
  exception 02. Define every address your client reads, even as `(static 0)`.
- **Word order.** If a float32 or u32 value looks wrong (huge, tiny or garbage), try
  `(word-order low-first)` or `--low-first` in the probe tool.
- **Test failures.** Add `(fault (kind drop) (percent 10))` to any profile to test how your
  client handles timeouts, or `(kind delay)` for slow answers.
