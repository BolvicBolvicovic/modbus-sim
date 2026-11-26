# Using a profile

A profile is a config file for one kind of device. Start one, read it with the probe tool, and change what you need.

The files in [`profiles/`](https://github.com/BolvicBolvicovic/mogbus/tree/main/profiles) are generic
examples of common device types. They are **not** copies of any one manufacturer's register map; adapt
the addresses and units to the device you want to imitate. The [catalog](profile-catalog.md) lists them.

## Start a profile

```bash
./mogbus profiles/power-meter.scm
./tools/modbus_probe.py input 0 2 --as float32      # -> [228.0]
```

Every profile starts with a comment that lists its register map, so you can read the file instead
of a manual.

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

## See also

- [Profile catalog](profile-catalog.md)
- [Write your own profile](write-your-own-profile.md)
- [Importing a register map](../import/import-overview.md)
