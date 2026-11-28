# Host writes in Python

Make the simulated station behave like a plant: the tank level follows the pump and the alarms follow the level, all driven from the program, and see how host writes differ from client writes.

A Modbus client sees a device only through its tables. The *process* behind them (water in, water
out, a motor drawing current) is yours to model, and the host side of the API is how you drive it.
Nothing is computed per request: values change when the host changes them (or, for ramps, when they
are read).

## Two kinds of write

The station has two sources of change, and mogbus keeps them apart:

- **Clients** change what a real client may change: setpoint, mode and run command. What they write is
  what \ref mogbus::core::Device::get_value "Device.get_value()" returns next, so the host reads
  its inputs the same way.
- **The host** (your process model) changes what the plant would measure, with
  \ref mogbus::core::Device::set_value "Device.set_value()". The point becomes *host-owned*: a ramp
  stops moving it, a client write to it is refused with exception 02, and `read_only` is ignored. This is what you
  want for the level, the flows and the alarms.

\ref mogbus::core::Device::write "Device.write()" and \ref mogbus::core::Device::read "Device.read()"
are the raw-register versions (they behave as a client request, without the network), useful to
check the encoding.

## See also

- [Host writes in C](../library/library-simulate.md)
- [Tutorial step 3: Make it behave](../tutorial/tutorial-simulate.md)
- \ref core.py "Python API reference"
